"""一键部署脚本：把本地已备份到 GitHub 的最新代码部署到服务器。

默认只打印执行计划（dry-run），确认无误后加 --confirm 实际执行：

  python tools/deploy_server.py                # 只看计划
  python tools/deploy_server.py --confirm      # 实际部署

认证方式（二选一）：
  1. 已配置 SSH key（推荐）；
  2. 环境变量 IOT_HOME_SSH_PASSWORD。

部署步骤：
  1. 服务器上 git 同步到 origin/codex/gateway-oled-wifi-checkpoint；
  2. 执行 MySQL 迁移 2026-09-22_command_lifecycle.sql（可重复执行）；
  3. 重新构建并启动 backend；
  4. 重启 grafana 以加载新面板（本地已加开关面板）；
  5. 只读验收：健康检查、received_at 字段、后端启动日志。

注意：服务器上 server/.env 不被 git 跟踪，部署不会改动密码配置。
"""

import os
import sys
import time

import paramiko

HOST = os.getenv("IOT_HOME_SSH_HOST", "8.163.110.27")
USER = os.getenv("IOT_HOME_SSH_USER", "root")
PASSWORD = os.getenv("IOT_HOME_SSH_PASSWORD") or None

REMOTE_DIR = "/root/iot-home"
COMPOSE_FILE = "docker-compose-minimal.yml"
BRANCH = "codex/gateway-oled-wifi-checkpoint"
MIGRATION = "server/mysql/migrations/2026-09-22_command_lifecycle.sql"
LOCAL_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class Deployer:
    def __init__(self, confirm: bool):
        self.confirm = confirm
        self.ssh = paramiko.SSHClient()
        self.ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())

    def connect(self):
        print(f"[连接] {USER}@{HOST}")
        self.ssh.connect(
            HOST,
            username=USER,
            password=PASSWORD,
            look_for_keys=True,
            allow_agent=True,
            timeout=20,
        )

    def run(self, command: str, timeout: int = 180, stdin_data=None):
        label = command if len(command) < 120 else command[:117] + "..."
        if not self.confirm:
            print(f"[计划] {label}")
            return "", ""

        print(f"[执行] {label}")
        _, stdout, stderr = self.ssh.exec_command(command, timeout=timeout)
        if stdin_data is not None:
            chan = stdout.channel
            chan.sendall(stdin_data)
            chan.shutdown_write()
        out = stdout.read().decode(errors="replace")
        err = stderr.read().decode(errors="replace")
        if out.strip():
            print(out.rstrip())
        if err.strip():
            print("  [stderr]", err.rstrip())
        return out, err

    def upload_migration(self):
        local_path = os.path.join(LOCAL_ROOT, *MIGRATION.split("/"))
        if not self.confirm:
            print(f"[计划] 通过 stdin 向 mysql 容器执行迁移: {MIGRATION}")
            return
        with open(local_path, "rb") as f:
            sql = f.read()
        command = (
            f"cd {REMOTE_DIR}/server && "
            f"docker compose -f {COMPOSE_FILE} exec -T mysql sh -c "
            "'exec mysql -u\"$MYSQL_USER\" -p\"$MYSQL_PASSWORD\" "
            "\"$MYSQL_DATABASE\"'"
        )
        self.run(command, timeout=120, stdin_data=sql)

    def deploy(self):
        dc = f"docker compose -f {COMPOSE_FILE}"

        self.run(
            f"cd {REMOTE_DIR} && git fetch origin && "
            f"git checkout {BRANCH} && "
            f"git reset --hard origin/{BRANCH}"
        )
        self.upload_migration()
        self.run(
            f"cd {REMOTE_DIR}/server && {dc} up -d --build backend",
            timeout=600,
        )
        self.run(
            f"cd {REMOTE_DIR}/server && {dc} restart grafana",
            timeout=120,
        )

        if self.confirm:
            print("\n[等待] 后端启动 25 秒...")
            time.sleep(25)
        self.verify()

    def verify(self):
        print("\n========== 验收（只读） ==========")
        self.run("curl -s http://localhost:8000/api/health")
        self.run(
            f"cd {REMOTE_DIR}/server && {('docker compose -f ' + COMPOSE_FILE)} "
            "exec -T mysql sh -c 'mysql -u\"$MYSQL_USER\" -p\"$MYSQL_PASSWORD\" "
            "\"$MYSQL_DATABASE\" -e \"SHOW COLUMNS FROM metrics LIKE "
            "\\'received_at\\';\"'"
        )
        self.run("docker logs --tail 15 iot-backend 2>&1")
        print(
            "\n[提示] 节点接回后，用 tools/analyze_integrity.py 复测，"
            "预期送达率接近 100%。"
        )

    def close(self):
        self.ssh.close()


def print_plan():
    print("=== DRY RUN：部署计划（加 --confirm 实际执行） ===")
    print(f"目标服务器 : {USER}@{HOST}")
    print(f"项目目录   : {REMOTE_DIR}")
    print(f"分支       : origin/{BRANCH}")
    print()
    print("1. git fetch / checkout / reset --hard 到远程分支")
    print(f"2. 执行 MySQL 迁移：{MIGRATION}（可重复执行）")
    print("3. docker compose up -d --build backend")
    print("4. docker compose restart grafana（加载开关面板）")
    print("5. 只读验收：health、received_at 列、后端日志")
    print()
    print("认证：SSH key，或环境变量 IOT_HOME_SSH_PASSWORD")
    print("提示：server/.env 不被 git 跟踪，部署不会改动密码配置。")


def main():
    confirm = "--confirm" in sys.argv
    if not confirm:
        print_plan()
        return
    deployer = Deployer(confirm)
    deployer.connect()
    try:
        deployer.deploy()
    finally:
        deployer.close()


if __name__ == "__main__":
    main()
