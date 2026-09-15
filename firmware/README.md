# 固件构建

目标芯片为 ESP32-S3，使用 ESP-IDF v6.1.0。准备 Git、Python 3.13+；Windows 还需安装 `uv`。首次配置需要联网，工具安装在 `firmware/.airdap-env/`。

## Windows（PowerShell）

从仓库根目录执行：

```powershell
cd firmware
uv run --locked python tools/setup.py
. .\get_env.ps1
idf.py build
```

## Linux（Bash）

从仓库根目录执行：

```bash
cd firmware
python3 tools/setup.py
. ./get_env.sh
idf.py build
```

`setup.py` 只需首次运行；以后在 `firmware/` 下激活环境，再执行 `idf.py build`。已有 ESP-IDF v6.1.0 时，可将其路径作为 `tools/setup.py` 的参数。

## 构建产物

- `build/airdap.bin`：APP 固件，用于 OTA。
- `build/bootloader/bootloader.bin`：bootloader。
- `build/partition_table/partition-table.bin`：分区表。
- `build/ota_data_initial.bin`：初始 OTA 数据。

需要包含上述四项的完整固件时，在已激活环境的 `firmware/` 目录执行：

```sh
idf.py merge-bin
```

生成 `build/merged-binary.bin`，完整烧录地址为 `0x0`。
