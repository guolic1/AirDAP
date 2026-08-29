# AirDAP GPIO 分配表

| GPIO | 信号/功能 | 方向/模式 | 有效电平与说明 |
|---:|---|---|---|
| 0 | `BOOT_KEY` | 输入 | 低有效；R1 10 kΩ 上拉，SW1 按下接地；兼作 ROM 下载启动键 |
| 3 | `TARGET_VTREF_ADC` | ADC 输入 | R7/R8 100 kΩ 等值分压，`VADC = Vtarget / 2` |
| 8 | `USB_VBUS_SENSE` | ADC/数字输入 | R18 150 kΩ、R19 220 kΩ 分压，`VADC = VBUS × 220 / 370` |
| 9 | `V_SOURCE_STATUS` | 输入/开漏输出 | 连接 TPS2116 `ST` 和 TPS22919 `ON`；写 1 释放，写 0 禁止目标供电；禁止推挽输出高 |
| 10 | `LED_STATUS` | 输出 | 红色状态 LED，低亮、高灭 |
| 11 | `LED_NET` | 输出 | 绿色网络 LED，低亮、高灭 |
| 12 | `TARGET_SWCLK_TCK` | 输出 | SWD SWCLK / JTAG TCK；启动时保持低 |
| 13 | `TARGET_SWDIO_TMS` | 双向 | SWDIO / JTAG TMS；传输方向由 GPIO14 控制 |
| 14 | `SWDIO_DIR` | 输出 | 低：目标→ESP32；高：ESP32→目标；R17 100 kΩ 下拉 |
| 17 | `TARGET_TX_TDI` | 输出 | 目标 UART TX / JTAG TDI |
| 18 | `TARGET_RX_TDO` | 输入 | 目标 UART RX / JTAG TDO |
| 19 | `USB_D-` | USB 双向 | 原生 USB D-，经 R15 22 Ω 连接 USB-C |
| 20 | `USB_D+` | USB 双向 | 原生 USB D+，经 R16 22 Ω 连接 USB-C |
| 41 | `TARGET_nRST` | 输出 | GPIO 高时 Q1 导通并拉低目标 nRST；GPIO 低时释放复位 |
