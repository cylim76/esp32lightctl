# 固件变体与可插拔边界

## Core 定义

Core 是办公室节能系统的长期稳定边界，包含 Wi-Fi、Web、NVS、Zigbee Coordinator、Aqara D1、LG 红外、DS3231、可信时间、本地定时任务、诊断和统一 Device Control Service。Core 不包含 BH1750、SHT31 或 RainMaker，也不链接三个扩展组件。

四种命名固件都以完整 Stage 5 Core 为基础：

| 变体 | Sensor 扩展 | RainMaker 扩展 | 当前软件状态 |
|---|---|---|---|
| `CORE` | 否 | 否 | Core 完整编译；硬件验证仍按测试记录 |
| `CORE_SENSOR` | 是 | 否 | 真实 BH1750 Lux、SHT31 温湿度/CRC/趋势、运行时独立启停；硬件 NOT TESTED |
| `CORE_CLOUD` | 否 | 是 | RainMaker Classic 1.17.0、Device Control、Claim/MQTT 路径已编译和软件测试；真实云端 NOT TESTED |
| `FULL` | 是 | 是 | 三个实验组件同时编译；传感器和真实云端仍为 HARDWARE/CLOUD NOT TESTED |

扩展代码分别位于 `extensions/bh1750`、`extensions/sht31` 和 `extensions/rainmaker`，三个目录是独立 ESP-IDF 组件及独立 Kconfig 开关。删除这些目录后，`CORE` 仍应独立构建。Core 的 `device_control.c` 不包含 RainMaker 头文件或回调；云扩展只能通过 `rainmaker_control_api_t` 调用统一控制服务。

## 构建

先运行 ESP-IDF 5.5.1 的 `export.bat`，再执行：

```bat
build-variant.bat CORE
build-variant.bat CORE_SENSOR
build-variant.bat CORE_CLOUD
build-variant.bat FULL
```

等价命令：

```bat
idf.py -B build-variant-CORE -D LIGHTGW_VARIANT=CORE build
```

每个变体必须使用独立 build 目录。`LIGHTGW_VARIANT` 会强制使用完整 Stage 5 Core 和对应 `sdkconfig.variant.*`；不要在同一 build 目录切换变体。

## 未启用模块的资源边界

- `CORE` 不把扩展目录加入 `EXTRA_COMPONENT_DIRS`，不会编译扩展源文件、执行初始化、创建扩展任务或分配扩展缓冲。
- BH1750 与 SHT31 在用户启用时各创建一个采样任务，通过 Core 共享 I²C 总线读取；禁用时终止任务并释放 device handle。每个传感器只保留 20 条 RAM 历史，配置写独立 `sensor_nvs`，采样不写 Flash。
- RainMaker 依赖固定在 `extensions/rainmaker/idf_component.yml`，没有加入 Core 的 `main/idf_component.yml`。CORE 不下载、不链接 RainMaker/TLS/MQTT 扩展依赖。
- 云端三路灯 ON/OFF 和全部 OFF 都经统一 Device Control Service；空调只暴露明确 OFF 触发器。灯状态只用未过期、未超时的 Zigbee 端点反馈同步；红外只报告“已发送/失败”，不虚报空调已关闭。
- RainMaker 1.17.0 没有可验证的完整运行时销毁/再次初始化路径。关闭开关会先持久化禁用状态，再返回 `restart_required`；受控重启后不启动云组件，避免强行释放仍在使用的 SDK 资源。

## I²C 所有权

`components/lightgw_i2c_bus` 是 Core 组件并拥有唯一 I²C0 总线。DS3231 和可选传感器只能附加设备句柄，不得删除或重新配置总线。移除 `extensions/bh1750` 和 `extensions/sht31` 后，DS3231 路径和 GPIO 配置保持不变。

## 配置与 Flash 兼容

核心配置继续使用既有主 NVS、`ir_data`、`runtime_nvs` 和 Zigbee 分区。扩展保留独立命名空间/版本：

- BH1750：`bh1750_ext`，版本 1，配置已写入 `sensor_nvs`。
- SHT31：`sht31_ext`，版本 1，配置已写入 `sensor_nvs`。
- RainMaker：`rainmaker_ext`，版本 1；运行时启用状态写入 `cloud_nvs`。Claiming/账户绑定凭据由 RainMaker SDK 自身管理，配置备份不得导出。

所有命名变体使用同一 `partitions_stage5.csv`。`sensor_nvs` 和 `cloud_nvs` 追加在所有既有分区之后，未移动 Wi-Fi、应用、Zigbee、coredump、红外或任务状态地址。刷入 `CORE` 不会擦除扩展分区，也不会清除核心数据。

任何未来分区变更必须先更新 `ROLLBACK.md`，提供完整 8 MiB 备份、分区级备份、迁移程序、断电恢复和回退演练；没有这些材料不得发布。

## 稳定标签规则

`core-stable-v1.0` 仅能在 CORE 完成目标板 Wi-Fi/Zigbee/IR/RTC/调度、断电恢复和长期运行验证后创建。仅编译或 QEMU 通过不得打 stable 标签。实验版本使用普通提交或预发布标签，不能覆盖已有 tag。
