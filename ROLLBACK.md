# 安全移除扩展与固件回退

## 原则

切换变体不是恢复出厂设置。正常刷写只写 bootloader、partition table 和应用，禁止执行 `erase-flash`，也禁止在启动异常时自动擦除 NVS。CORE 必须能够忽略并保留 Sensor/Cloud 扩展数据。

## 回退前备份

关闭串口监视器，确认端口后先保存完整 8 MiB Flash：

```bat
python -m esptool --chip esp32c6 -p COM5 -b 460800 read_flash 0x0 0x800000 backup-full-8mb.bin
```

建议另存关键分区，当前 Stage 5/命名变体地址如下：

```bat
python -m esptool --chip esp32c6 -p COM5 read_flash 0x9000   0x6000  backup-nvs.bin
python -m esptool --chip esp32c6 -p COM5 read_flash 0x310000 0x40000 backup-zb-storage.bin
python -m esptool --chip esp32c6 -p COM5 read_flash 0x350000 0x1000  backup-zb-fct.bin
python -m esptool --chip esp32c6 -p COM5 read_flash 0x361000 0x10000 backup-ir-data.bin
python -m esptool --chip esp32c6 -p COM5 read_flash 0x371000 0x10000 backup-runtime-nvs.bin
python -m esptool --chip esp32c6 -p COM5 read_flash 0x381000 0x10000 backup-sensor-nvs.bin
python -m esptool --chip esp32c6 -p COM5 read_flash 0x391000 0x10000 backup-cloud-nvs.bin
```

备份后计算 SHA-256，并记录当前 Git 提交、固件版本、分区表和串口日志。没有备份时不要跨分区版本降级。

## FULL/SENSOR/CLOUD 切换到 CORE

1. 使用同一提交构建 `CORE`。
2. 比较 `build-variant-CORE/partition_table/partition-table.bin` 与来源变体，必须一致。
3. 执行普通 `idf.py -B build-variant-CORE -p COM5 flash`，不得执行 `erase-flash`。
4. 启动后检查 Wi-Fi、Zigbee 网络、Aqara 映射、红外码、DS3231、56 条任务和 `runtime_nvs` 状态。
5. Sensor/Cloud 分区保留但不初始化；以后刷回扩展变体仍可迁移其数据。

## 删除扩展源码

删除 `extensions/bh1750`、`extensions/sht31` 或 `extensions/rainmaker` 前，先确认 CORE 的 CMake 不引用该目录，然后运行完整 `check-all.bat`。删除扩展不得修改 `components/lightgw_i2c_bus`、Core 分区地址或 Device Control Service。

## 降级到历史固件

历史 Stage 1～5 可能使用较短的分区表或旧配置结构。较短分区表本身不会擦除尾部 Flash，但旧固件可能无法理解新配置版本，因此不能假设可直接安全降级。

- 同分区、向后兼容的数据版本：可先在备用板演练，再普通刷写。
- 分区或数据版本不兼容：优先恢复对应版本的完整 8 MiB 备份。
- 只恢复单个 NVS/Zigbee 分区前必须确认加密设置、地址和大小完全一致。
- 禁止把其他设备的 NVS、Zigbee 或加密数据写入本设备。

完整镜像恢复命令：

```bat
python -m esptool --chip esp32c6 -p COM5 -b 460800 write_flash 0x0 backup-full-8mb.bin
```

恢复后验证 SHA-256、启动日志和所有核心数据。若写入中断，保留现场日志并从已校验的完整备份重新恢复；不要让固件自行清空配置。

## Tag 回退

稳定标签只指向经过硬件验收的不可变提交。推荐流程：

```bat
git fetch --tags
git switch --detach core-stable-v1.0
build-variant.bat CORE
```

不要移动或覆盖已有 stable tag。当前尚未完成硬件验收，因此不得把本次模块化架构提交标记为 `core-stable-v1.0`。
