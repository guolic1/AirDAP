# AirDAP 网络 USB/IP 桥接

主机后台程序把 AirDAP 的认证网络服务转换为本机 USB/IP 1.1.1 设备。
USB/IP 客户端将它挂入操作系统后，调试软件使用 CMSIS-DAP v2，串口软件使用
Windows `COMx` 或 Linux `/dev/ttyACM*`。这里的烧录指通过 CMSIS-DAP/SWD
给目标芯片烧录；不提供 DAPLink Mass Storage 拖拽烧录磁盘。

需要本机 Web 管理、USB/BLE 配网、OTA，以及 Windows Service / Linux systemd
开机自启时，使用仓库根目录的 [`host/`](../host/README.md)。下文的命令行桥接和旧后台入口仍保留。

## 当前固件网络功能

| 入口 | 功能 | 协议 |
| --- | --- | --- |
| TCP 3260 | CMSIS-DAP、目标复位/供电控制、AirDAP 自身网络 OTA | TLS 1.3 PSK-DHE + AirDAP v1 帧 |
| TCP 3261 | 目标 UART 状态、TX 所有权、参数设置、分块读写 | 同上；CONTROL_REQUEST/RESPONSE |
| mDNS / UDP 5353 | 在线发现、设备身份、DAP/UART 端口 | `_airdap._tcp` |
| 本机 TCP 3240（新增） | 虚拟 CMSIS-DAP v2 + CDC ACM 复合 USB 设备 | USB/IP，仅监听 `127.0.0.1` |

源代码约定见 `firmware/components/network_dap/`、`network_uart/` 和 `discovery/`。
3260/3261 都不是原始字节透传端口；桥接复用现有 TLS、HELLO、AUTH、帧序列检查。
DAP 连接先认证，UART 连接携带同一个 owner token 加入；每两秒发送 KEEPALIVE。
只有 USB/IP IMPORT 时才连接设备，列出虚拟设备不会连接 AirDAP 或占用所有权。

## 运行前提

- Python 3.13+，SSL 支持 `set_psk_client_callback`。Windows 使用仓库 `uv` 环境；
  不增加 Python 依赖，不需要修改/重刷固件。
- 已完成 Wi-Fi 和网络认证配对，拥有 `airdap-pair.py` 生成的凭据文件。
  Linux 凭据权限必须为 `0600`；Windows 使用仅允许本人读取的目录/ACL。
- 固件在线且允许 NETWORK DAP。若 AirDAP 的物理 USB 仍连接，默认策略优先 USB；
  可使用已配置的 `dap-network` / `dap-toggle` 按键操作选择 NETWORK。
- Linux 内核需要 `vhci_hcd` 和 USB/IP 客户端；Windows 需要 USB/IP VHCI 客户端驱动。
  [usbip-win2](https://github.com/vadimgrn/usbip-win2) 提供 Windows 客户端及签名驱动。
  安装其发布的签名包，按上游安装要求操作。桥接脚本不会安装驱动或更改系统签名策略。
  `usbipd-win` 的导出设备功能不能代替 Windows VHCI 导入客户端。

USB/IP 本机入口没有独立认证；同一机器上能连接该端口的进程可使用已经配对的设备。
不要使用端口转发把 3240 暴露到局域网。远端通信继续强制使用原有 TLS-PSK。

## 前台启动与挂载

在仓库根目录运行，用实际设备地址和凭据路径替换占位值：

```powershell
uv run --locked python firmware/tools/airdap-usbip.py AIRDAP_HOST --credential C:\private\airdap.json
```

Linux 可使用同一 `uv run` 命令，或已有 Python 3.13+ 环境：

```sh
python3 firmware/tools/airdap-usbip.py AIRDAP_HOST --credential "$HOME/.config/airdap/credential.json"
```

程序默认监听 `127.0.0.1:3240`，bus ID 固定为 `1-1`，一次只允许一个导入者。
`--port` 可改变本机 USB/IP 端口，`--dap-port` / `--uart-port` 可覆盖设备端口，
`--timeout` 设置网络操作超时（默认 5 秒）。使用非默认 USB/IP 端口时，还需按客户端
自身选项设置同一端口；以下命令均使用默认 3240。

Windows 若已运行 `usbipd`，它可能占用 3240。保留该服务，给桥接指定其他空闲端口，
例如启动时加 `--port 3242`，后台安装时加 `-Port 3242`；usbip-win2 对应的挂载命令为
`usbip.exe -t 3242 attach -r 127.0.0.1 -b 1-1 --once`。`--once` 禁止该客户端在挂载失败后
自动重复尝试，便于先检查桥接日志。

Linux 在另一个终端挂载：

```sh
sudo modprobe vhci_hcd
usbip list -r 127.0.0.1
sudo usbip attach -r 127.0.0.1 -b 1-1
usbip port
```

Windows 安装 VHCI 客户端后，用管理员终端挂载：

```powershell
usbip.exe list -r 127.0.0.1
usbip.exe attach -r 127.0.0.1 -b 1-1
```

设备包含接口 0（CMSIS-DAP Bulk，端点 `01/81`）和接口 1/2（CDC ACM，
端点 `82/03/83`）。BOS 和 Microsoft OS 2.0 描述符为 DAP 接口声明 WinUSB，
CDC 使用系统串口驱动。虚拟 USB serial 为 `ADP-xxxxxxxxxxxx-NET`，用于区分
同一台设备的物理 USB 和网络枚举；网络 HELLO、DAP_Info 仍报告固件的实际身份和版本。
开发 VID/PID 保持 `303A:4021`，不代表已分配的产品标识。

挂载后先用 `pyocd list` 或现有调试工具确认探针，选择带 `-NET` 的 serial。
OpenOCD 使用 `interface/cmsis-dap.cfg`，目标配置沿用目标芯片原本的配置；
烧录软件本身仍需支持该芯片。串口软件直接选择系统分配的 COM/ttyACM 端口。
Linux 非 root 使用者还需要发行版对应的 USB udev 权限和串口组权限。

卸载时使用 attach 输出或客户端状态列出的**实际虚拟端口号**，它不是 bus ID `1-1`：

```sh
usbip port
sudo usbip detach -p PORT_NUMBER
```

```powershell
usbip.exe detach -p PORT_NUMBER
```

解除挂载会关闭两个认证连接并释放固件所有权。Ctrl-C/终止后台程序也会断开虚拟设备；
客户端可能仍保留失效的虚拟端口，需要先 detach 再 attach。

## Windows 后台运行

在仓库根目录用 PowerShell 注册当前用户登录时运行的计划任务：

```powershell
./firmware/tools/airdap-usbip-task.ps1 -Action Install -DeviceHost AIRDAP_HOST -Credential C:\private\airdap.json
./firmware/tools/airdap-usbip-task.ps1 -Action Status
```

安装命令解析 `uv` 管理的 Python，直接用 `pythonw.exe` 无窗口启动桥接；登录时不会
运行 `uv` 或安装依赖。任务使用当前用户交互会话，注销后不继续运行。它不会自动挂载
虚拟设备，启动后仍执行上面的 USB/IP attach。每个用户只注册一个任务，已有配置不会
被覆盖。更换仓库路径、Python 环境或凭据路径时先 Remove，再 Install。

日志位于 `%LOCALAPPDATA%\AirDAP\usbip.log`，每份最多 2 MiB，保留两份备份。
`Status` 显示计划任务状态；设备是否挂载应通过 USB/IP 客户端、设备管理器和日志确认。

```powershell
./firmware/tools/airdap-usbip-task.ps1 -Action Stop
./firmware/tools/airdap-usbip-task.ps1 -Action Start
./firmware/tools/airdap-usbip-task.ps1 -Action Remove
```

Remove 只移除当前用户的桥接任务，保留凭据和日志；不会删除驱动或其他 USB/IP 设备。

## Linux 后台运行

提供 `firmware/tools/airdap-usbip.service` 用户级 systemd unit。
先在 `~/.config/airdap/usbip.env` 填写四个值（路径必须是绝对路径，不能写 `$HOME`）：

```ini
AIRDAP_PYTHON=/absolute/path/to/python3
AIRDAP_SCRIPT=/absolute/path/to/AirDAP/firmware/tools/airdap-usbip.py
AIRDAP_HOST=airdap-device.local
AIRDAP_CREDENTIAL=/absolute/private/path/credential.json
```

该 env 文件只记录路径和主机名，不记录 PSK；实际凭据仍使用配对文件。
使用 `uv run --locked python -c 'import sys; print(sys.executable)'` 可以确认解释器绝对路径。
安装并启动：

```sh
mkdir -p ~/.config/systemd/user
cp firmware/tools/airdap-usbip.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now airdap-usbip.service
systemctl --user status airdap-usbip.service
journalctl --user -u airdap-usbip.service -f
```

然后按上文 attach。服务跟随用户 systemd 会话启动，不自行启用 linger，也不自动
申请 root 权限或重连虚拟设备。停止/禁用使用：

```sh
systemctl --user disable --now airdap-usbip.service
```

## 行为边界与验证

- CMSIS-DAP 单包上限 508 字节，DAP 请求按序执行，响应有界排队；串口读写以
  最多 256 字节网络块转换，波特率、停止位、校验和数据位遵循固件范围。
- CDC DTR 低时只缓存 line coding，不申请 TX；DTR 高时申请并设置 UART。
  关闭 COM 会阻止后续虚拟 CDC 写入，但固件没有 RELEASE_TX 操作，因此 UART TX
  所有权保留到 USB/IP detach。要把 TX 交给物理 USB 或另一客户端，必须 detach。
  RTS 只缓存，不驱动额外引脚；不支持 BREAK、硬件流控或真实 modem 信号。
- 不映射物理 USB debug shell，不添加 Mass Storage、HID 或 SWO 端点。
  复位/供电 CONTROL 和 AirDAP 自身 OTA 不新增独立虚拟 USB 功能；继续使用现有工具。
- 认证撤销、网络超时、协议错误会断开整个虚拟设备。程序继续监听，
  修复原因后重新 attach。传输期间不自动重连、重发 DAP 或 UART 写入。
  正在执行的 OUT/control 被取消时也断开设备，因为目标可能已经收到操作。
- UART RX 环满时固件会丢弃新数据，桥接日志报告累计丢字节数。串口应用仍可读取
  剩余字节，不会因此断开正在使用的 DAP；日志中的 overflow 意味着串口数据不完整。
- 协议单元测试中的模拟后端只证明转换和 USB/IP 协议；这些测试不能证明实物 SWD 烧录、
  UART 电气时序、无线可靠性或 Windows 驱动兼容性。

从 `firmware/` 运行主机测试：

```sh
cmake -S test/unit/airdap_usbip -B build-host/airdap_usbip
cmake --build build-host/airdap_usbip
ctest --test-dir build-host/airdap_usbip --output-on-failure
```

Windows 干净 shell 没有 `nmake` 时，在仓库根目录使用 Ninja，并让 `uv` 提供 Python 环境：

```powershell
uv run --locked cmake -G Ninja -S firmware/test/unit/airdap_usbip -B firmware/build-host/usbip-ninja
cmake --build firmware/build-host/usbip-ninja
ctest --test-dir firmware/build-host/usbip-ninja --output-on-failure
```

实物验收需明确设备及烧录授权：在 Windows、Linux 分别确认枚举、DAP_Info、
目标芯片烧录后读回校验、COM/ttyACM 多种波特率双向传输，以及拔网/恢复后 detach/attach。
已有 `firmware/test/hil/wireless_pyocd.py` 使用直接 TCP 适配器；它的成功不能替代
通过本机虚拟 USB 枚举的烧录验收。

仓库另提供两个本机驱动集成测试，后端完全模拟，不需要 AirDAP 或凭据：

```powershell
uv run --locked python firmware/test/integration/usbip_windows.py
```

```sh
sudo modprobe vhci_hcd cdc_acm
sudo python3 firmware/test/integration/usbip_linux.py
```

Windows 测试需要已安装的 usbip-win2。Linux 测试直接连接空闲的 VHCI 端口，
不依赖 `usbip` 命令。两者使用临时本机 TCP 端口，只清理自己创建的挂载，验证
真实系统驱动枚举、USB Bulk DAP_Info 和 CDC 二进制回环；不是实物烧录测试。

USB/IP 报文依据 [Linux 内核协议文档](https://www.kernel.org/doc/html/latest/usb/usbip_protocol.html)，
Windows 导入方式依据 [usbip-win2 使用说明](https://github.com/vadimgrn/usbip-win2#use-usbipexe-to-attach-remote-devices)。
