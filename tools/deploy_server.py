"""一键部署脚本：把 GitHub 上的最新代码部署到服务器。

用法：

  python tools/deploy_server.py                 # 只看计划（不连接）
  python tools/deploy_server.py --confirm       # 实际部署

常用选项：

  --host HOST               服务器地址（默认 8.163.110.27）
  --user USER               SSH 用户（默认 root）
  --fingerprint SHA256:...  期望的主机公钥指纹（也可用
                            IOT_HOME_SSH_FINGERPRINT）

认证：SSH key（推荐）或环境变量 IOT_HOME_SSH_PASSWORD。

安全设计：

- 主机身份必须验证：从系统 known_hosts 加载，未知主机直接拒绝；
  提供 --fingerprint 时还会核对实际指纹是否完全一致，防止中间人；
- 每条远端命令都检查退出码，失败立即中止并返回非零；
- 部署前检查服务器 git 工作区是否有本地改动，默认中止，
  确认覆盖可设置 IOT_HOME_ALLOW_DIRTY=1；
- 记录部署前 SHA，便于快速回滚；
- 迁移直接使用服务器刚拉取代码中的文件，保证迁移与代码同一提交；
- 不读取/打印任何密码，不改动服务器 server/.env。
"""

import argparse
import json
import os
import sys
import time

import paramiko

REMOTE_DIR = "/root/iot-home"
COMPOSE_FILE = "docker-compose-minimal.yml"
BRANCH = "codex/gateway-oled-wifi-checkpoint"
MIGRATION_PATH = "server/mysql/migrations/2026-09-22_command_lifecycle.sql"

HEALTH_TIMEOUT_S = 90
HEALTH_POLL_S = 5


class FingerprintKeyPolicy(paramiko.MissingHostKeyPolicy):
    """核对主机实际指纹是否等于期望值；否则拒绝。"""

    def __init__(self, expected_fingerprint: str):
        self.expected = expected_fingerprint.strip()

    def missing_host_key(self, client, hostname, key):
        # OpenSSH 风格指纹：SHA256: + base64（无填充）。
        import base64

        actual = "SHA256:" + base64.b64encode(
            key.get_fingerprint()
        ).decode().rstrip("=")
        if actual != self.expected.rstrip("="):
            raise paramiko.SSHException(
                f"主机指纹不匹配：期望 {self.expected}，实际 {actual}"
            )


class DeployError(RuntimeError):
    """部署过程中的确定性失败。"""


class Deployer:
    def __init__(self, args):
        self.args = args
        self.ssh = paramiko.SSHClient()
        # 始终加载系统已知主机密钥。
        self.ssh.load_system_host_keys()

        fingerprint = (
            args.fingerprint
            or os.getenv("IOT_HOME_SSH_FINGERPRINT")
            or None
        )
        if fingerprint:
            self.ssh.set_missing_host_key_policy(
                FingerprintKeyPolicy(fingerprint)
            )
        else:
            # 未知主机直接拒绝，不做无条件信任。
            self.ssh.set_missing_host_key_policy(paramiko.RejectPolicy())

    def connect(self):
        print(f"[连接] {self.args.user}@{self.args.host}")
        self.ssh.connect(
            self.args.host,
            username=self.args.user,
            password=os.getenv("IOT_HOME_SSH_PASSWORD") or None,
            look_for_keys=True,
            allow_agent=True,
            timeout=20,
        )
        # 连接成功后再次核对实际指纹（known_hosts 中可能有多个密钥）。
        if self.args.fingerprint or os.getenv("IOT_HOME_SSH_FINGERPRINT"):
            expected = (
                self.args.fingerprint
                or os.getenv("IOT_HOME_SSH_FINGERPRINT")
            )
            sock = self.ssh.get_transport().sock
            remote_key = self.ssh.get_transport().get_remote_server_key()
            import base64

            actual = "SHA256:" + base64.b64encode(
                remote_key.get_fingerprint()
            ).decode().rstrip("=")
            if actual.rstrip("=") != expected:
                raise DeployError(
                    f"主机指纹不匹配：期望 {expected}，实际 {actual}"
                )

    def run(
        self,
        command,
        timeout=180,
        stdin_data=None,
        check=True,
    ):
        label = command if len(command) < 120 else command[:117] + "..."
        print(f"[执行] {label}")
        _, stdout, stderr = self.ssh.exec_command(command, timeout=timeout)
        if stdin_data is not None:
            stdout.channel.sendall(stdin_data)
            stdout.channel.shutdown_write()
        out = stdout.read().decode(errors="replace")
        err = stderr.read().decode(errors="replace")
        exit_code = stdout.channel.recv_exit_status()
        if out.strip():
            print(out.rstrip())
        if err.strip():
            print("  [stderr]", err.rstrip())
        if check and exit_code != 0:
            raise DeployError(
                f"远程命令失败(exit={exit_code})：{label}\n{err}\n{out}"
            )
        return out, err, exit_code

    def preflight(self):
        print("\n----- 1/5 部署前检查 -----")
        out, _, _ = self.run(f"cd {REMOTE_DIR} && git rev-parse HEAD")
        old_sha = out.strip()
        print(f"部署前 SHA：{old_sha}（回滚：git reset --hard {old_sha}）")

        out, _, _ = self.run(
            f"cd {REMOTE_DIR} && git status --porcelain", check=True
        )
        dirty = out.strip()
        if dirty:
            if os.getenv("IOT_HOME_ALLOW_DIRTY") == "1":
                print("[警告] 服务器工作区有本地改动，已按 "
                      "IOT_HOME_ALLOW_DIRTY=1 继续：")
                print(dirty)
            else:
                raise DeployError(
                    "服务器 git 工作区有本地改动，默认中止部署：\n"
                    f"{dirty}\n确认覆盖请设置 IOT_HOME_ALLOW_DIRTY=1。"
                )
        return old_sha

    def sync_code(self):
        print("\n----- 2/5 同步代码 -----")
        self.run(f"cd {REMOTE_DIR} && git fetch origin")
        self.run(f"cd {REMOTE_DIR} && git checkout {BRANCH}")
        self.run(f"cd {REMOTE_DIR} && git reset --hard origin/{BRANCH}")

    def run_migration(self):
        print("\n----- 3/5 执行数据库迁移 -----")
        # 直接使用服务器刚拉取代码中的迁移文件（同一提交）。
        command = (
            f"cd {REMOTE_DIR} && "
            f"docker compose -f server/{COMPOSE_FILE} exec -T mysql sh -c "
            "'exec mysql -u$MYSQL_USER -p$MYSQL_PASSWORD $MYSQL_DATABASE' "
            f"< {MIGRATION_PATH}"
        )
        self.run(command, timeout=180)

    def rebuild_backend(self):
        print("\n----- 4/5 重建并启动 backend -----")
        self.run(
            f"cd {REMOTE_DIR}/server && "
            f"docker compose -f {COMPOSE_FILE} up -d --build backend",
            timeout=600,
        )
        self.run(
            f"cd {REMOTE_DIR}/server && "
            f"docker compose -f {COMPOSE_FILE} restart grafana",
            timeout=120,
        )

    def wait_healthy(self):
        print("\n----- 5/5 等待后端健康 -----")
        deadline = time.time() + HEALTH_TIMEOUT_S
        last_error = None
        while time.time() < deadline:
            try:
                out, _, code = self.run(
                    "curl -fsS http://localhost:8000/api/health",
                    check=False,
                )
                if code == 0:
                    health = json.loads(out)
                    if health.get("status") != "healthy":
                        raise DeployError("health status 非 healthy")
                    if not health.get("mqtt_connected"):
                        raise DeployError("backend 未连接 MQTT")
                    print("[OK] backend healthy，MQTT 已连接")
                    return
            except (json.JSONDecodeError, DeployError) as exc:
                last_error = exc
            time.sleep(HEALTH_POLL_S)
        raise DeployError(f"后端在 {HEALTH_TIMEOUT_S}s 内未恢复健康："
                          f"{last_error}")

    def verify_schema(self):
        print("\n====== 只读验收 ======")
        out, _, _ = self.run(
            f"cd {REMOTE_DIR}/server && docker compose -f {COMPOSE_FILE} "
            "exec -T mysql sh -c 'exec mysql -u$MYSQL_USER "
            "-p$MYSQL_PASSWORD $MYSQL_DATABASE'",
            stdin_data=b"SHOW COLUMNS FROM metrics LIKE 'received_at';\n",
        )
        if "received_at" not in out:
            raise DeployError("验收失败：metrics.received_at 列不存在")
        print("[OK] metrics.received_at 已存在")
        self.run("docker logs --tail 15 iot-backend 2>&1", check=False)
        print(
            "\n[提示] 节点接回后运行 tools/analyze_integrity.py 复测，"
            "预期送达率接近 100%。"
        )

    def deploy(self):
        self.preflight()
        self.sync_code()
        self.run_migration()
        self.rebuild_backend()
        self.wait_healthy()
        self.verify_schema()

    def close(self):
        self.ssh.close()


def print_plan(args):
    print("=== 部署计划（加 --confirm 实际执行） ===")
    print(f"目标服务器 : {args.user}@{args.host}")
    print(f"项目目录   : {REMOTE_DIR}")
    print(f"分支       : origin/{BRANCH}")
    print(f"主机指纹   : {args.fingerprint or '校验系统 known_hosts'}")
    print()
    print("1. 检查工作区改动并记录旧 SHA（可回滚）")
    print("2. git fetch / checkout / reset --hard")
    print("3. 执行数据库迁移（服务器仓库内文件，可重复执行）")
    print("4. up -d --build backend，restart grafana")
    print("5. 轮询健康检查并验收 received_at")
    print()
    print("认证：SSH key，或 IOT_HOME_SSH_PASSWORD")
    print("安全：主机密钥强制验证，命令失败即中止")


def build_parser():
    parser = argparse.ArgumentParser(description="部署 IoT-Home 到服务器")
    parser.add_argument("--confirm", action="store_true",
                        help="实际执行部署（默认只打印计划）")
    parser.add_argument("--host", default="8.163.110.27")
    parser.add_argument("--user", default="root")
    parser.add_argument("--fingerprint", default=None,
                        help="期望主机指纹 SHA256:...")
    return parser


def main():
    args = build_parser().parse_args()
    if not args.confirm:
        print_plan(args)
        return

    deployer = Deployer(args)
    deployer.connect()
    try:
        deployer.deploy()
    except Exception as exc:
        print(f"\n[部署失败] {exc}")
        raise SystemExit(1)
    finally:
        deployer.close()


if __name__ == "__main__":
    main()
