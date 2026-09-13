# AirDAP 主机服务

Windows / Linux 后台服务、本机 Web 管理页，以及网络 CMSIS-DAP + CDC USB/IP 桥接。
本目录集中存放主机服务文件，复用 `firmware/tools/` 已有的配网、凭据、USB/IP 和 OTA 协议实现。
不需要新增 Python 生产依赖。Python 需为支持 TLS-PSK 的 3.13 或更新版本。

## 功能

| 功能 | 入口与限制 |
| --- | --- |
| USB/IP 桥接 | 网络 DAP TCP 3260 + 目标 UART TCP 3261；本机虚拟 CMSIS-DAP 和 COM / ttyACM |
| 自动恢复 | 保存已启用状态，服务重启后恢复监听；可选每 15 秒重新挂载，旧 DAP/UART 请求不重放 |
| 设备发现 | USB 枚举和 BLE 配网广播；过滤虚拟 `-NET` 设备，必须明确选择物理设备编号 |
| 设备信息 | 网络 TLS-PSK HELLO 读取版本、UUID、能力；USB 读取固件、网络、UART、OTA 诊断 |
| Wi-Fi 配置 | BLE Security 2 或 USB debug shell；只有 DHCP 联网确认后报告成功 |
| 网络凭据 | 导入现有 JSON，或在 BLE / USB 配网会话中生成、保存并写入设备；重复操作复用本机凭据 |
| OTA | 网络 CONTROL 或物理 USB；上传、显示版本/大小/SHA-256、确认后写非活动槽并重启 |
| 系统服务 | Windows SCM 自动启动及失败恢复；Linux systemd enable、Restart=on-failure |

不提供目标芯片的浏览器烧录界面；目标烧录仍由 Keil、OpenOCD、pyOCD 等通过虚拟 CMSIS-DAP 完成。
不开放网络 debug shell，也不提供任意命令执行接口。

## 前台运行

从仓库根目录执行：

```powershell
uv sync --locked
uv run --locked python host/airdap-service.py --http-port 8080 --usbip-port 3242
```

打开 [本机管理页](http://127.0.0.1:8080)。HTTP 和 USB/IP 均固定监听 `127.0.0.1`。
`--no-http` 关闭管理页，使用先前保存的设备配置恢复桥接。
`--data-dir` 指定状态目录；前台默认 Windows `%LOCALAPPDATA%\AirDAP\service`，
Linux `~/.local/share/AirDAP/service`。同一数据目录有进程锁，不允许同时运行两个实例。

设备首次使用：

1. 查找 USB 设备或扫描已开启配网广播的 BLE 设备，选择设备，保存其编号和 IP/mDNS 主机名。
2. 选择蓝牙或 USB，点击“开始配网”。蓝牙需先在设备上开启配网模式，USB 需连接物理设备。
3. 连接后，可分别点击“建立网络连接凭据”和“连接 Wi-Fi”。前者生成并保存本机凭据，再校验设备返回的指纹；后者让设备扫描 Wi-Fi，显示 SSID、RSSI、信道、加密方式和 BSSID，点击热点后输入密码。USB 需要独立 debug shell，目前仍仅支持 ASCII SSID/密码。
   配置完成或失败都保留窗口，可以重试、扫描或继续配对；只有“取消配网”结束会话，已保存配置不撤销。正在执行的写入须完成后才能取消。
4. 确认设备 IP。mDNS 无法解析时填写实际 IP。点击网络读取验证，再启动桥接。
5. 虚拟 USB 序列号为 `ADP-xxxxxxxxxxxx-NET`；目标 COM 编号由系统分配。配置、配对、升级前先停止桥接。

配网会话需要本次新增固件接口；旧固件会明确报不支持，不会伪造已连接。
蓝牙每 25 秒续期 120 秒的设备会话；服务退出或链路丢失后设备超时清理，网页保持窗口并提示错误。
服务运行期间刷新页面会恢复当前会话。服务重启不自动恢复配网或重放凭据/Wi-Fi 写入。
网络凭据生成后先私密保存，再发送给设备；设备确认失败时保留同一份凭据供重试，成功状态以指纹匹配为准。
烧录器同时占用物理 USB DAP 时，网络路由可能被固件优先级拒绝；按设备既有配置切换到网络 DAP 模式。

BLE 复用 ESP-IDF 和下载的 Espressif `network_provisioning` 客户端，默认寻找仓库配置。
隔离 worktree 可指定已有组件位置，不必复制或重新下载固件构建目录：

```powershell
uv run --locked python host/airdap-service.py `
  --idf-path C:/path/to/esp-idf `
  --provisioning-dir C:/path/to/network_provisioning/tool/esp_prov
```

## Windows Service

先安装项目依赖及签名的 `usbip-win2` VHCI 驱动/客户端。
已有 `usbipd-win` 不等于导入客户端；其 3240 端口无需停止。

在**管理员 PowerShell** 中，从仓库根目录执行：

```powershell
./host/install-windows.ps1 -Action Install -HttpPort 8080 -UsbipPort 3242
./host/install-windows.ps1 -Action Status
./host/install-windows.ps1 -Action Stop
./host/install-windows.ps1 -Action Start
./host/install-windows.ps1 -Action Remove
```

安装程序将服务代码、现有 Python 解释器与环境依赖复制到 `%ProgramFiles%\AirDAP`，
以 LocalSystem 注册原生服务 `AirDAP`，自动启动，并设置异常退出恢复。
解释器来自 `uv run --locked python`；也可用 `-PythonPath` 指定已装好仓库依赖的环境。
代码和数据目录 ACL 仅授予 SYSTEM 与 Administrators，避免特权服务执行普通用户可修改的代码。
服务数据位于 `%ProgramData%\AirDAP`，日志 `service.log` 轮转为 2 MiB × 3。
`-NoHttp` 可安装不启用 Web 的服务。

`-IdfPath` / `-ProvisioningDirectory` 可指定 BLE 组件来源。安装时复制需要的客户端及
protocomm Python 文件，运行服务无需访问用户 ESP-IDF 工作目录。
缺少组件时安装仍可运行 USB 配网、桥接与 OTA，但 BLE 配网会明确报告未就绪。
Windows 服务账户下的蓝牙可用性受适配器及 Windows 权限影响，须在目标机器确认。

安装前停止占用同一 HTTP / USB/IP 端口的前台实例。可使用 `-HttpPort` / `-UsbipPort`
或 `-InstallDirectory` / `-DataDirectory` 自定义 Program Files / ProgramData 下的专用子目录。
已有服务和安装目录不会静默覆盖，已有数据目录必须具有本安装器的 AirDAP 标记。
升级服务程序时停止并移除旧服务注册，再安装到新的专用目录，并保留同一数据目录。
移除操作只删除服务注册，保留程序、配置和凭据。

## Linux systemd

需要 systemd、系统 Python 3.13+、`uv`、`usbip` 客户端与 `vhci_hcd` 内核模块。
BLE 还需要工作的 BlueZ 服务/适配器。安装脚本使用已锁定的项目依赖，不修改系统 pip 环境。

```sh
sudo env PATH="$PATH" python3 host/install-linux.py install --python /usr/bin/python3.14
sudo python3 host/install-linux.py status
sudo python3 host/install-linux.py stop
sudo python3 host/install-linux.py start
sudo python3 host/install-linux.py remove
```

程序安装到 `/opt/airdap`，数据保存在 `/var/lib/airdap`，服务为 `airdap.service`。
安装完成执行 `systemctl enable --now`，开机加载 VHCI 并启动。
为访问 USB/IP 的 sysfs、物理 USB 和蓝牙，服务以 root 运行；使用私有数据权限及只读系统目录限制。
`--http-port` / `--usbip-port` / `--no-http` 同样可用于安装。
卸载仅移除服务注册；更换程序版本前需人工检查并保留旧安装目录，脚本不会覆盖它。

```sh
systemctl is-enabled airdap.service
journalctl -u airdap.service
sudo tail -n 80 /var/lib/airdap/service.log
```

## OTA 与失败处理

选择文件后点击“上传并检查镜像”。上传只在内存暂存一份镜像，不写设备，USB 桥接监听期间也可以上传。
真正点击“确认设备并升级”前，需要停止 USB 桥接并关闭烧录器和串口工具。
页面显示 ESP32-S3 应用描述符版本、大小和 SHA-256；
执行时再次核对设备编号、镜像摘要和确认字段。实际容量和完整镜像合法性仍由设备 OTA 管理器验证。
网络 OTA 使用既有 `airdap-network-update.py` 的容量、偏移、提交及槽位/版本/启动确认检查。
USB OTA 使用既有 `airdap-update.py`，重启后检查同一物理序列号和目标版本。
USB QUERY 不提供槽位/启动确认字段，所以 USB 结果不会冒充网络 OTA 的完整启动确认。
这是固件开发接口，不提供新增签名、加密镜像或防降级保障。

每次只执行一个设备操作。服务停止会等待已经开始的操作完成；不要强制结束正在升级的进程。
提交成功但重连失败时显示失败/状态不确定，不自动重新写入。USB/网络 OTA 写入错误由现有客户端
尝试 ABORT，网络凭据和 Wi-Fi 操作也不会自动重试。
掉线仅触发虚拟 USB 重新挂载，因此烧录器/COM 程序可能需要重新打开设备。
停止只清理指向本服务 `127.0.0.1:端口/1-1` 的挂载，不清理其他 USB/IP 设备。

Wi-Fi 密码仅保留在本次操作内存中，不进入配置/命令参数/日志。网络 PSK 独立保存在
`credentials/<device-id>.json`，POSIX 权限 0600；配置使用原子替换。
浏览器 API 检查 Host、Origin、Fetch Metadata 和本次进程令牌；不启用 CORS。
这些检查防止跨站页面操作本机服务，不是同机用户之间的认证边界。

## 验证

```powershell
cmake -S host/tests -B build/host-service-tests -G Ninja
cmake --build build/host-service-tests
ctest --test-dir build/host-service-tests --output-on-failure
```

Linux 另运行真实进程的 SIGTERM / 恢复 / `--no-http` 检查：

```sh
python3 host/tests/test_process.py
```

测试隔离物理设备与网络上游，不会配网、旋转凭据或烧录。Windows 本机服务注册需要管理员权限；
USB/BLE 配网持久性、实物 OTA 及开机后蓝牙行为需要在明确选定设备上另做验收。
