# Windows USB 烧录与串口调试

## 准备

1. 安装 ESP-IDF 5.5.1，并运行其 `export.bat`。
2. 使用支持数据传输的 USB 线连接 `ESP32-C6-DevKitC-1-N8`。
3. 在设备管理器确认串口，例如 `COM7`。
4. 首次上板只烧 Stage 1。

## 推荐命令

```bat
flash.bat COM7 1
monitor.bat COM7 1
```

依次验证后再升级：

```bat
flash.bat COM7 2
flash.bat COM7 3
flash.bat COM7 4
flash.bat COM7 5
```

对应原生 `idf.py` 命令：

```bat
idf.py -B build-stage1 -D LIGHTGW_STAGE=1 -p COM7 flash
idf.py -B build-stage2 -D LIGHTGW_STAGE=2 -p COM7 flash
idf.py -B build-stage3 -D LIGHTGW_STAGE=3 -p COM7 flash
idf.py -B build-stage4 -D LIGHTGW_STAGE=4 -p COM7 flash
idf.py -B build-stage5 -D LIGHTGW_STAGE=5 -p COM7 flash
```

命名变体使用完整 Stage 5 Core 和同一兼容分区表。先构建，再普通烧录；切换变体不得执行 `erase-flash`：

```bat
build-variant.bat CORE
idf.py -B build-variant-CORE -D LIGHTGW_VARIANT=CORE -p COM7 flash

build-variant.bat CORE_SENSOR
idf.py -B build-variant-CORE_SENSOR -D LIGHTGW_VARIANT=CORE_SENSOR -p COM7 flash

build-variant.bat CORE_CLOUD
idf.py -B build-variant-CORE_CLOUD -D LIGHTGW_VARIANT=CORE_CLOUD -p COM7 flash

build-variant.bat FULL
idf.py -B build-variant-FULL -D LIGHTGW_VARIANT=FULL -p COM7 flash
```

从实验变体退回 CORE 前必须按 `ROLLBACK.md` 备份 Flash；当前尚未完成目标板回退演练。

串口监控：

```bat
idf.py -B build-stage1 -p COM7 monitor
```

按 `Ctrl+]` 退出。若自动下载失败，按住 BOOT、短按 RESET、松开 BOOT 后重试。

## 直接烧录地址

每个 `build-stageN` 目录都包含：

```text
0x0000  bootloader/bootloader.bin
0x8000  partition_table/partition-table.bin
0x10000 esp32c6_light_gateway.bin
```

仓库提交的预编译副本位于 `firmware\stageN`，地址相同。烧录前可使用 `certutil -hashfile 文件 SHA256` 与 `firmware\SHA256SUMS.txt` 核对。

示例：

```bat
python -m esptool --chip esp32c6 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m 0x0 build-stage1\bootloader\bootloader.bin 0x8000 build-stage1\partition_table\partition-table.bin 0x10000 build-stage1\esp32c6_light_gateway.bin
```

## 数据保留与清除

直接从 Stage 1 升级到 Stage 2/3/4/5 时主 NVS 会保留。Stage 5 必须同时烧录它自己的 `partition-table.bin`，`flash.bat COM7 5` 会自动完成；新增的 `ir_data` 位于原 coredump 分区之后，不改变原 NVS、应用、Zigbee 或 coredump 地址。需要全新测试时才执行：

```bat
idf.py -p COM7 erase-flash
```

该命令会清除 Wi-Fi、管理凭据、设备诊断、端点映射、Zigbee 网络以及 Stage 5 学习到的红外码，且不可恢复。不要在已有网络上随意执行。

## 必须保留的日志

从复位第一行开始保存串口输出，至少应包含固件版本/Stage/复位原因、DHCP IPv4、mDNS 主机名、Wi-Fi 断线 reason、Coordinator 建网、Permit Join、设备 announce、`DIAG` 端点/Cluster、控制 TX、状态反馈、Free Heap 和错误信息。编译日志与串口日志应分开归档。
