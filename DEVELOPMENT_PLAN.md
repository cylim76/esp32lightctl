# Phase A～E 开发计划

优先级始终是 Core 可靠性，其次才是实验扩展。每个 Phase 都必须更新测试、执行受影响 Stage/变体构建并保留硬件 `NOT TESTED` 边界。

## Phase A — Core 数据与控制可靠性

- 完成 NVS 非破坏初始化、小粒度调度状态、Zigbee 多设备状态隔离、回读超时和缓存有效期。
- 持续验证 Wi-Fi、Web、Aqara、红外、RTC、可信时间和 56 条任务。
- 扩展不得改变 Device Control、核心配置或既有分区地址。

## Phase B — 模块边界与安全移除

- 维护 CORE、CORE_SENSOR、CORE_CLOUD、FULL 四种构建。
- 验证分别删除 BH1750、SHT31、RainMaker 三个扩展目录后 CORE 仍能编译，DS3231 仍使用共享 I²C。
- 执行 FULL→CORE 数据保留、断电恢复和回退演练。

## Phase C — 实验环境传感器

- BH1750 与 SHT31 驱动、短期趋势和 Web 展示全部位于 Sensor 扩展。
- 设备缺失、地址冲突或读取失败不得阻止 Core；禁止长期大缓冲和高频 Flash 写入。
- 使用 `sensor_nvs`、独立版本和迁移测试，不写 Core 配置。

## Phase D — 实验 RainMaker

- RainMaker TLS/MQTT/配网依赖只存在于云扩展。
- 控制回调只能调用 Device Control Service；Core 不包含 RainMaker 头文件、状态或重连逻辑。
- 使用 `cloud_nvs`、独立版本和有限重试；云不可用不影响本地调度。

## Phase E — 全量资源与长期验收

- 对四种变体执行 Flash/RAM、堆峰值、栈水位、无线共存和 7～30 天稳定性测试。
- 实际演练扩展移除、FULL→CORE、配置迁移、备份恢复及旧稳定标签回退。
- 只有 CORE 的目标板验收全部通过后，才创建不可移动的 `core-stable-v1.0` 标签。
