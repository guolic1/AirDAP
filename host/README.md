# AirDAP 原生主机服务

Rust 实现的 Windows / Linux 服务。发布程序内嵌 Web 页面、BLE Security 2、USB 与网络协议；
运行时不需要 ESP-IDF 或额外的 OpenSSL DLL。
`host/` 统一存放服务、Web 资源和安装工具。

## 功能

| 功能 | 行为 |
| --- | --- |
| USB/IP | TCP 3260/3261 的 TLS-PSK DAP 与 UART 映射为 CMSIS-DAP 和 CDC COM/ttyACM |
| 自动恢复 | 保存桥接启用状态；进程重启恢复，挂载失败 1 秒后重试；断线不重放旧请求 |
| Web | 固定监听本机 `127.0.0.1`，同源校验和每次启动生成的 API token |
| 配网 | USB / 蓝牙选择、持续会话、设备扫描 Wi-Fi、SSID/RSSI/信道/加密/BSSID、选择热点连接 |
| 网络凭据 | 兼容原有 JSON；先私密保存再写设备，失败重试使用相同凭据，检查返回指纹 |
| 设备信息 | 网络 HELLO 查询版本/UUID/能力；物理 USB 固定诊断命令 |
| OTA | 上传检查版本/大小/SHA-256，确认后写非活动槽；网络重启后核对槽位、版本和启动确认 |
| 服务 | Windows SCM / Linux systemd，开机自启、异常退出重启，停止时等待当前设备操作完成 |

配网成功或失败后窗口都保持打开，直到“取消配网”。蓝牙每 25 秒续期，设备租期为 120 秒。
USB 配网仍遵循固件限制：需要独立 debug shell，SSID/密码仅支持可打印 ASCII。
USB OTA 可验证重新枚举和版本，但该 USB QUERY 没有网络 QUERY 的槽位确认字段。
BLE 使用固件既有的公开开发配网口令；本次迁移没有改变固件的安全边界。
没有新增网络 debug shell 端口。

## 构建与前台运行

在完整仓库的 `host/` 内使用 Rust 1.93 或更新版本，依赖由 `Cargo.lock` 锁定：

```sh
cargo build --release --locked
./target/release/airdap-service --data-dir ./local-data --http-port 8080 --usbip-port 3242
```

Windows 运行 `target\release\airdap-service.exe`，其余参数相同。
构建需要 C 编译工具、Perl 和 make（vendored OpenSSL/libusb）；Linux 还需系统的蓝牙服务 BlueZ。
Windows 可用 MSVC 构建环境，或在 Linux 中用 `x86_64-pc-windows-gnu` + MinGW 交叉编译：

```sh
rustup target add x86_64-pc-windows-gnu
CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER=x86_64-w64-mingw32-gcc \
  cargo build --release --locked --target x86_64-pc-windows-gnu
```

打开 <http://airdap.localhost:8080>。用 `--http-port 18080` 选择其他端口后，访问
`http://airdap.localhost:18080`。现代浏览器将 `.localhost` 解析到本机，无需修改 hosts 或公网 DNS；
服务仍只监听 `127.0.0.1`，原有 `127.0.0.1` / `localhost` 地址继续可用。
如果客户端不支持 `.localhost` 解析，可使用相同端口的 `127.0.0.1` 地址。
`--no-http` 关闭 Web；`--usbip-executable` 指定系统 USB/IP 客户端。
前台默认数据目录：Windows `%LOCALAPPDATA%\AirDAP\service`，Linux `$XDG_DATA_HOME/AirDAP/service`
或 `~/.local/share/AirDAP/service`。`service.log` 记录操作结果，轮转为 2 MiB × 3，不记录请求内容或凭据。
Linux 发布版使用构建机器的 glibc 基线，分发到较旧发行版时应在最旧目标环境重新构建。

## Windows 服务安装

推荐下载单文件 `AirDAP-Manager.exe`，双击并确认 Windows 管理员权限提示。
管理器内嵌本版本的 Rust 服务，使用 Windows 自带的 .NET Framework 图形界面，运行时不需要
PowerShell 脚本或单独解压服务程序。管理器仅在打开窗口时运行，关闭后不影响后台服务。

| 管理器操作 | 行为 |
| --- | --- |
| 安装服务 | 选择 Web / USB/IP 端口、是否开启 Web、是否开机自启；校验端口后安装并启动 |
| 更新服务 | 使用当前管理器内嵌的服务程序，保留原端口、自启设置、运行/停止状态和设备数据 |
| 应用设置 | 修改端口、Web 开关与开机自启；正在运行时停止并重新启动，失败时尝试恢复原设置 |
| 启动 / 停止 / 重启 | 操作 Windows SCM；停止等待当前设备任务结束，不强制终止 OTA |
| 卸载服务 | 注销服务并删除服务 EXE，保留配置、凭据、日志和不认识的文件 |
| 打开管理页面 | 打开已安装服务的 `http://airdap.localhost:所选端口` |

更新已运行的服务若启动失败，会尝试恢复旧程序并重新启动；恢复失败会显示错误并保留可用的诊断信息。
系统命令超时导致结果不确定时，保留程序/备份并要求检查状态，不报告成功或自动重复操作。
已停止的服务更新后仍保持停止，其新版本的启动情况需在点击“启动”后确认。
更新通过下载新版管理器并点击“更新服务”完成，当前不联网自动下载更新。
管理器是便携工具，不会将自身注册为后台进程或添加到“已安装的应用”；卸载通过管理器完成。

旧 `AirDAPNative` 服务可用“更新服务”迁移为 `AirDAP`。迁移保留原 `AirDAPNative` 程序/数据目录，
不复制或重新生成凭据；新安装使用 `AirDAP` 目录。同名的其他程序、自定义启动参数或同时存在两份
服务时会拒绝修改，须先核对旧安装。图形管理器与下述旧脚本不同，允许在卸载后复用权限正确的保留数据目录。

USB/IP 驱动不捆绑在管理器内；界面显示客户端/驱动检测结果，并提供官方签名驱动下载入口。
本次生成的管理器尚未进行代码签名。

命令行安装方式仍可使用：

先安装签名的 usbip-win2 导入驱动和客户端；usbipd-win 不能替代导入客户端。
停止占用相同端口的旧实例。在**管理员 PowerShell** 中执行：

```powershell
./install-windows.ps1 -Action install -Binary ./target/release/airdap-service.exe
./install-windows.ps1 -Action status
./install-windows.ps1 -Action stop
./install-windows.ps1 -Action start
./install-windows.ps1 -Action remove
```

交叉编译时将 `-Binary` 指向 `target/x86_64-pc-windows-gnu/release/airdap-service.exe`。
可设置 `-HttpPort`、`-UsbipPort` 和 `-NoHttp`。
例如安装到 Web 端口 18080：

```powershell
./install-windows.ps1 -Action install -Binary ./target/release/airdap-service.exe -HttpPort 18080
```

安装后访问 `http://airdap.localhost:18080`。端口范围为 1–65535，请选择未被占用且与 USB/IP 不同的端口。
服务名为 `AirDAP`，以 LocalSystem 运行，程序放入 `%ProgramFiles%\AirDAP`，
数据放入 `%ProgramData%\AirDAP`，ACL 仅授权 SYSTEM 与 Administrators。
命令行安装脚本拒绝覆盖已有服务、程序或数据目录；其 `remove` 只注销服务并保留程序及凭据。
Windows 服务账户下的蓝牙访问取决于适配器和权限，须在目标机器验证。

### 构建和验证 Windows 管理器

先构建 Windows 服务，再在 `host/` 中执行：

```powershell
.\manager\build.ps1 -ServiceBinary .\target\release\airdap-service.exe
```

默认输出到仓库 `build/releases/windows-x64/AirDAP-Manager.exe`。Windows x64 的 .NET Framework
`csc.exe` 直接编译，未添加 NuGet 或其他第三方生产依赖。

不修改系统的测试（参数保护、真实进程参数传递、内嵌程序、图形窗口和只读服务状态）：

```powershell
.\manager\build.ps1 -ServiceBinary .\target\release\airdap-service.exe -OutputDirectory ..\build\manager-check -Test
```

**仅在没有 AirDAP 服务及安装/数据目录的干净 Windows 测试环境中，以管理员权限执行：**

```powershell
..\build\manager-check\AirDAP-Manager.Tests.exe --system-test
```

该测试会实际安装和卸载服务，验证 Web 就绪、端口/自启修改、更新、旧名称迁移和数据保留，
并注入不能启动的测试程序验证更新/迁移回退。发现已有安装或数据会拒绝运行；失败时保留现场。
测试创建的服务不会配置设备或启用 USB 桥接。`*Tests.exe`、`AirDAP-Failure*.exe` 和预览图仅用于测试，
不得加入面向用户的发行包。

## Linux 服务安装

先安装发行版的 `usbip`、BlueZ，并确认内核支持 `vhci_hcd`。在本目录执行：

```sh
sudo sh install-linux.sh install ./target/release/airdap-service
sudo sh install-linux.sh status
sudo sh install-linux.sh stop
sudo sh install-linux.sh start
sudo sh install-linux.sh remove
```

安装时可依次指定 Web 和 USB/IP 端口，例如
`sudo sh install-linux.sh install ./airdap-service 18080 3242`，之后访问
`http://airdap.localhost:18080`。省略端口时仍使用 8080 / 3242。

注册 `airdap.service`，程序 `/opt/airdap/airdap-service`，数据 `/var/lib/airdap`。
使用 root 是为了访问 VHCI、USB 和系统蓝牙。systemd 限制系统文件写入；管理接口仅监听回环地址。
用 `systemctl edit airdap.service` 设置端口：先用空 `ExecStart=` 清除原值，再填写完整命令。
卸载保留数据和程序。更新现有安装时，先停止服务，再由管理员替换精确的可执行文件并启动。

## 旧版数据迁移

1. 停止旧桥接和服务，保留原数据目录作为备份。
2. 启动服务时直接使用原 `--data-dir`，或在两边均已停止时复制 `config.json` 与 `credentials/`。
   Linux 凭据文件必须为 0600、目录为 0700；Windows 服务数据须保留安装器设置的 ACL。
3. 使用原端口运行服务。已有 `bridge_enabled=true` 会恢复桥接；首次试运行可先改为 false。
4. 读取设备信息并核对后启用桥接。如需回退，使用旧版本发行包及备份配置。

同一数据目录不能同时运行多个服务实例，`service.lock` 进程锁用于阻止重复启动。
正式服务名统一为 `AirDAP` / `airdap.service`。安装器不会覆盖同名的其他程序或不认识的安装；
迁移前先停止并使用旧版工具注销旧服务，保留数据备份。
Linux 若存在 `airdap-native.service`，先停用旧单元并备份旧数据；新安装器会拒绝并存安装。
旧单元可留作备份文件（移出 `/etc/systemd/system/`），执行 `systemctl daemon-reload` 后安装新服务，
停止新服务再按上述权限约定将配置和凭据迁入 `/var/lib/airdap`，确认新服务正常后再清理旧程序。
Web 页面、JSON API 和凭据格式保持兼容。

## 验证

```sh
cargo fmt --check
cargo clippy --locked --all-targets -- -D warnings
cargo test --locked --all-targets
cargo build --locked --bins --examples
```

上述命令覆盖 Rust 单元和协议测试；管理器验证见上方 Windows 管理器章节。
当前不再提供独立的 HTTP、TLS-PSK/USB-IP 和 OTA 进程模拟测试脚本；
进程间交互和设备写入仍需结合实际服务与硬件验收，不能由这些单元测试代替。

2026-09-14 实机验证：Windows VHCI 成功枚举 CMSIS-DAP 与 COM，DAP_Info 读取通过，
COM 打开/关闭及 57600→115200 波特率配置通过；蓝牙与 USB 的配网握手、热点扫描、
凭据指纹确认和取消会话通过。蓝牙闲置 144 秒后仍可扫描和写入凭据，设备再次重启后的
首次连接也通过。Linux 原生服务通过真实 TLS-PSK 与设备通信，经 USB/IP 客户端读取 DAP_Info 成功。
网络 OTA 确认切换到原非活动槽且已确认启动；USB OTA 确认
断开、重新枚举和版本，随后用物理 USB 诊断核对镜像状态为 valid。使用同一份已验证固件测试，
没有改变固件版本。设备重启约 2.16 秒后旧 USB/IP 会话断开、约 2.37 秒开始重挂载，之后 DAP 可读；
这不是物理断电到 PnP 删除的精确时延测量。

实机发现并修复 Windows 驱动自动移除与显式卸载的竞态、USB OTA 重新枚举期间临时通信错误，
以及 Windows 蓝牙首次连接的 GATT 会话建立顺序。失败的卸载仅在确认本服务导出已消失后视为完成；
USB 枚举错误不视为断开证明，也不自动重放 OTA；Windows 使用显式 GATT 会话保持连接，取消时释放。

管理器已通过 Windows 编译、参数/归属校验、真实进程参数传递、内嵌服务和图形界面只读验证。
当前会话没有管理员权限，尚未执行管理器 `--system-test`；真实 SCM 安装、更新回退、名称迁移和卸载
仍需管理员环境验收。尚未执行 Linux systemd 实际安装、Linux VHCI 导入、实物 Wi-Fi 密码写入、
目标芯片烧录或 UART 线缆回环；也未做破坏性回滚故障注入。
这些场景不能由成功构建、模拟测试、DAP 信息读取或 COM 打开代替证明。

## 本次资源测量

Windows 本机，Rust release 服务读取设备 HELLO 后，Web 开启、桥接未挂载、无浏览器轮询；
测量时长为 20.01 秒：

| 指标 | 测量结果 |
| --- | ---: |
| 工作集 RSS | 13.34 MiB |
| 私有内存 | 2.53 MiB |
| 线程数 | 4 |
| 单核 CPU 均值 | 计时精度内 0% |

短时 CPU 采样的 0% 不表示完全没有开销；未测量实物烧录、持续串口或 BLE 扫描峰值。
