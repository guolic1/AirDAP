# AirDAP

基于 ESP32-S3 的无线调试器，支持 CMSIS-DAP 调试烧录和串口通信。
通过 USB 直连，或使用 Windows / Linux 主机服务将网络设备映射为 USB 调试器和串口。

支持蓝牙 / USB 配网、Wi-Fi 扫描、设备信息查看及 OTA 升级。

## 快速开始

从 [Releases](https://github.com/guolic1/AirDAP/releases/latest) 下载服务管理器和固件：

- **Windows**：双击服务管理器安装服务，可设置端口和开机自启。默认管理地址为 `http://airdap.localhost:8080`。
- **Linux**：按 [主机服务说明](host/README.md) 构建并安装 systemd 服务。
- **固件**：`app.bin` 用于 OTA；`full.bin` 用于首次烧录或恢复，写入地址为 `0x0`，会清除配网数据。

Windows 的 USB/IP 映射需安装 [usbip-win2 驱动和客户端](https://github.com/vadimgrn/usbip-win2/releases)。

## 项目目录

- [firmware/](firmware/README.md)：ESP-IDF 固件与构建说明。
- [host/](host/README.md)：Rust 主机服务、Web 管理页和服务安装工具。
- [circuit/](circuit/)：KiCad 原理图与 PCB。
- [docs/hardware-pinmap.md](docs/hardware-pinmap.md)：硬件引脚约定。

## 许可证

[MIT](LICENSE)。第三方依赖遵循各自的许可证。
