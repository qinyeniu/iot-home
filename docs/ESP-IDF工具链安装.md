# ESP-IDF 工具链安装记录（v5.5.4）

> 日期：2026-08-22　|　状态：**已验证**（hello_world 编译通过，待实机烧录）

## 一、安装结果总览

| 项目 | 值 |
|------|-----|
| 安装管理器 | EIM v0.18.0（乐鑫官方 ESP-IDF Installation Manager） |
| ESP-IDF | **v5.5.4**（ESP-Zigbee-SDK v2.x 官方推荐版本） |
| 安装位置 | `C:\Espressif`（固件在 `v5.5.4\esp-idf`，工具链在 `tools`） |
| 目标芯片 | `esp32c6`（只装 RISC-V 工具链，未装其他芯片工具链） |
| Python 环境 | `C:\Espressif\tools\python\v5.5.4\venv`（Python 3.14） |
| VS Code 插件 | `espressif.esp-idf-extension` v2.2.0 |
| 复现配置 | [tools/eim_config.toml](../tools/eim_config.toml)（EIM 导出） |

## 二、为什么选 v5.5.4

ESP-Zigbee-SDK（官方仓库 README）明确：v2.x 是"新设计 + 生产"推荐的 Zigbee 栈，
推荐搭配 **ESP-IDF v5.5.4**。本项目的网关（协调器）和终端（Zigbee 节点）都基于此组合。

## 三、安装步骤（可复现）

### 1. 下载 EIM

- GitHub 官方：`https://github.com/espressif/idf-im-ui/releases/download/v0.18.0/eim-cli-windows-x64-v0.18.0.exe`（22.6MB）
- 国内镜像直链（推荐，实测 2.8 秒下完）：`https://dl.espressif.cn/github_assets/espressif/idf-im-ui/releases/download/v0.18.0/eim-cli-windows-x64-v0.18.0.exe`

> 注意：`dl.espressif.cn/dl/eim` 页面目前 **404**（v0.18.0 未同步），GitHub 直连极慢，
> 用上面的 `github_assets` 镜像最快。

### 2. 执行安装（PowerShell）

```powershell
cd C:\
.\eim-cli-windows-x64-v0.18.0.exe install `
  -i v5.5.4 -t esp32c6 -p C:\Espressif `
  -r false `
  -m https://dl.espressif.cn/github_assets `
  --idf-mirror https://gitee.com --repo-stub EspressifSystems/esp-idf `
  --pypi-mirror https://pypi.tuna.tsinghua.edu.cn/simple `
  -a true
```

参数说明：

- `-m`：**工具链**下载走乐鑫国内镜像（快）
- `--idf-mirror` + `--repo-stub`：**ESP-IDF 主仓库**从码云官方镜像克隆（乐鑫 .cn 服务器不提供 git 服务）
- `-r false`：跳过子模块（原因见踩坑 3）
- `--pypi-mirror`：Python 依赖走清华镜像
- `-a true`：自动补装缺失的前置组件（Git/Python 等）

### 3. 补子模块内容

码云没有子模块镜像，必须从官方源码包解出全部 23 个子模块目录：

```powershell
curl -L -o C:\Espressif\dist\esp-idf-v5.5.4.zip `
  https://dl.espressif.cn/github_assets/espressif/esp-idf/releases/download/v5.5.4/esp-idf-v5.5.4.zip

tar -xf C:\Espressif\dist\esp-idf-v5.5.4.zip `
  -C C:\Espressif\v5.5.4\esp-idf --strip-components=1 `
  "esp-idf-v5.5.4/components"  # 或逐个列出子模块路径
```

### 4. 跳过子模块 git 检查

构建系统会校验子模块 git 状态（会尝试联网初始化 → 必失败），官方开关：

```powershell
[Environment]::SetEnvironmentVariable("IDF_SKIP_CHECK_SUBMODULES","1","User")
```

并已在激活脚本 `C:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1` 末尾追加了
`$env:IDF_SKIP_CHECK_SUBMODULES = "1"`，之后每次激活环境自动生效。

## 四、踩坑记录

| # | 现象 | 原因 | 解决 |
|---|------|------|------|
| 1 | EIM 官网下载页 404 | v0.18.0 未同步到 .cn 的 `/dl/eim` 路径 | 用 `github_assets` 镜像直链 |
| 2 | 安装时报"repository not found" | EIM 用 git 克隆，乐鑫 .cn 镜像不提供 git 仓库 | 主仓库改 Gitee 镜像 |
| 3 | 子模块拉取弹登录框 / 认证失败 | 码云无子模块仓库或不允许按提交号拉取 | `-r false` + 从官方 zip 解出内容 |
| 4 | cmake 报 `Git submodule init failed` | 子模块内容缺 git 元数据，构建尝试联网初始化 | `IDF_SKIP_CHECK_SUBMODULES=1` |
| 5 | 报 `xtensa-esp32-elf-gcc` 找不到 | 目标芯片默认成了 esp32（我们只装了 riscv 工具链） | `$env:IDF_TARGET="esp32c6"` 后 build |
| 6 | `idf.py set-target` 后 sdkconfig 未生成 | 构建目录残留坏状态 | 删除 build 目录后 `IDF_TARGET` 环境变量方式 |

## 五、验证结果

```text
idf.py --version          → ESP-IDF v5.5.4
riscv32-esp-elf-gcc -v    → 14.2.0 (crosstool-NG esp-14.2.0_20260121)
hello_world 编译           → 通过（esp32c6，1072 步，bin 0x27db0 字节）
```

**待办**：开发板插上后执行 `idf.py -p COMx flash` + `idf.py -p COMx monitor`，
串口看到 `Hello world!` 即完成整链实机验收。

## 六、日常使用

1. 打开桌面"ESP-IDF PowerShell"快捷方式（或 `.\C:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1` 激活）
2. 新工程：`idf.py create-project <名字>`；设目标：`$env:IDF_TARGET="esp32c6"`
3. 编译 / 烧录 / 监视：
   ```powershell
   idf.py build
   idf.py -p COM5 flash
   idf.py -p COM5 monitor        # Ctrl+] 退出
   ```
4. VS Code：插件已装，会自动读取 `C:\Espressif\tools\eim_idf.json`；
   命令面板输入 `ESP-IDF: Create Project` 可图形化建工程
5. **注意**：烧录 ESP-IDF 会覆盖板上的 MicroPython。备份都在仓库里：
   - MicroPython 固件：`tools/firmware/ESP32_GENERIC_C6-20260406-v1.28.0.bin`
   - 终端演示程序：`tools/tests/node_demo.py`（可随时重新刷回）

## 七、遗留说明

- 用户目录下若存在旧的 `eim_config.toml`（内含 `path = C:\esp`），EIM 会读取它；
  用 `-p C:\Espressif` 显式覆盖即可，或删除该文件。
- `C:\Espressif\dist\esp-idf-v5.5.4.zip`（1.83GB）保留作子模块/离线重装的素材库；
  磁盘紧张时可删除，代价是以后补子模块要重新下载。
- EIM 曾生成一条指向 `C:\esp` 的旧安装记录，已手工清理（备份 `eim_idf.json.bak`）。
