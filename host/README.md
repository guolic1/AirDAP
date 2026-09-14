# AirDAP 原生主机服务

Rust 实现的 Windows / Linux 服务。发布程序内嵌 Web 页面、BLE Security 2、USB 与网络协议；
运行时不需要 Python、ESP-IDF、`IDF_PATH` 或额外的 OpenSSL DLL。
`host/` 统一存放服务、Web 资源和安装工具，旧 Python 服务实现已移除。

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
服务名为 `AirDAPNative`，以 LocalSystem 运行，程序放入 `%ProgramFiles%\AirDAPNative`，
数据放入 `%ProgramData%\AirDAPNative`，ACL 仅授权 SYSTEM 与 Administrators。
安装器拒绝覆盖已有服务、程序或数据目录；删除服务只注销服务并保留程序及凭据。
Windows 服务账户下的蓝牙访问取决于适配器和权限，须在目标机器验证。

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

注册 `airdap-native.service`，程序 `/opt/airdap-native/airdap-service`，数据 `/var/lib/airdap-native`。
使用 root 是为了访问 VHCI、USB 和系统蓝牙。systemd 限制系统文件写入；管理接口仅监听回环地址。
用 `systemctl edit airdap-native.service` 设置端口：先用空 `ExecStart=` 清除原值，再填写完整命令。
卸载保留数据和程序。更新现有安装时，先停止服务，再由管理员替换精确的可执行文件并启动。

## 从 Python 迁移

1. 停止旧桥接和服务，保留原数据目录作为备份。
2. 启动原生服务时直接使用原 `--data-dir`，或在两边均已停止时复制 `config.json` 与 `credentials/`。
   Linux 凭据文件必须为 0600、目录为 0700；Windows 服务数据须保留安装器设置的 ACL。
3. 使用原端口运行原生服务。已有 `bridge_enabled=true` 会恢复桥接；首次试运行可先改为 false。
4. 读取设备信息并核对后启用桥接。如需回退，使用旧版本发行包及备份配置；当前仓库不再包含 Python 服务。

两种实现使用兼容的 `service.lock` 进程锁，同一数据目录不能同时运行。
系统服务安装使用独立名称，不会覆盖原 `AirDAP` / `airdap.service`；仍需避免端口和设备所有权冲突。
Web 页面、JSON API 和凭据格式保持兼容；旧 CLI 的 `--idf-path` 与 `--provisioning-dir` 已不需要。

## 验证

```sh
cargo fmt --check
cargo clippy --locked --all-targets -- -D warnings
cargo test --locked --all-targets
cargo build --locked --bins --examples
python3 tests/network_usbip.py -v
python3 tests/service_http.py -v
python3 tests/service_ota.py -v
```

进程测试需要 Python 3.13+ TLS-PSK，仅用于验证，不是服务依赖。
默认运行 `target/debug/airdap-service` 和 `target/debug/examples/bridge`（Windows 自动添加 `.exe`）。
自定义构建目录时设置 `AIRDAP_NATIVE_SERVICE` / `AIRDAP_NATIVE_BRIDGE` 指向对应构建产物。
OTA 模拟器占用本机 3260，运行前确保空闲；WSL 镜像网络可能与 Windows 测试产生端口冲突，
可使用 Linux 独立网络命名空间测试。Windows 的强制终止不能模拟 SCM 停止，SIGTERM 排空用例仅在 Linux 执行。

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

尚未执行 Windows SCM / Linux systemd 实际安装、Linux VHCI 导入、实物 Wi-Fi 密码写入、
目标芯片烧录或 UART 线缆回环；也未做破坏性回滚故障注入。
这些场景不能由成功构建、模拟测试、DAP 信息读取或 COM 打开代替证明。

## 本次资源测量

Windows 本机，两个实现分别读取同一设备的 HELLO 后，Web 开启、桥接未挂载、无浏览器轮询；
测量 20.01 秒，Python 包含启动器及实际服务进程，Rust 为 release 构建：

| 指标 | Rust | Python |
| --- | ---: | ---: |
| 工作集 RSS | 13.34 MiB | 40.82 MiB |
| 私有内存 | 2.53 MiB | 22.82 MiB |
| 线程数 | 4 | 8 |
| 单核 CPU 均值 | 计时精度内 0% | 0.078% |

此场景 RSS 约减少 67%。短时 CPU 采样的 0% 不表示完全没有开销；未测量实物烧录、持续串口或 BLE 扫描峰值。
