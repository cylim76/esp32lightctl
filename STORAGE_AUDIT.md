# Flash 与 NVS 存储审计

审计日期：2026-11-03。目标：ESP32-C6-DevKitC-1-N8（8 MB Flash）。本文件区分源码确定事实、软件模拟和仍需硬件测量的项目。

## 结论

本次发现并修复了三个确定问题：

1. `main.c` 在默认 NVS 返回 `ESP_ERR_NVS_NO_FREE_PAGES` 或 `ESP_ERR_NVS_NEW_VERSION_FOUND` 时会自动执行 `nvs_flash_erase()`，可能一次清空 Wi-Fi、管理员账户、设备映射和定时配置。
2. `ir_storage.c` 对红外 NVS 采用相同的自动擦除策略，可能无确认地删除已学习关机码。
3. `scheduler.c` 每次状态变化整体重写 `exec_v1`（56 项状态），每条操作日志又整体重写 `log_v1`；失败重试会产生不必要的 Flash 写放大。

修复后，初始化错误只进入 `DEGRADED` 状态并保留原数据；只有管理员认证后且输入完整确认文本 `ERASE_ALL_USER_DATA` 才能调用集中式恢复出厂设置。调度状态独立存放在 `runtime_nvs`，每个任务使用 `ex00`～`ex55` 单独记录；最近 8 条调度日志只保存在固定 RAM 环形缓冲区。旧 `exec_v1` 和 `log_v1` 均只读且保留，不自动删除。

## 分区审计

Stage 1～4 的地址如下；新增 `runtime_nvs` 位于所有既有分区之后，没有移动既有 NVS、应用、Zigbee 或 coredump 地址。

| 分区 | 地址 | 大小 | 用途 | 写入特征 |
|---|---:|---:|---|---|
| `nvs` | `0x9000` | 24 KiB | Wi-Fi、管理账户、设备名、定时配置、应用 Zigbee 设备快照 | 配置真实变化或设备发现/映射变化时 |
| `phy_init` | `0xF000` | 4 KiB | ESP-IDF PHY 数据 | 由 ESP-IDF 管理 |
| `factory` | `0x10000` | 3 MiB | 单个 factory 应用 | 固件烧录时 |
| `zb_storage` | `0x310000` | 256 KiB | ZBOSS 网络持久化 | 由 Zigbee 栈管理 |
| `zb_fct` | `0x350000` | 4 KiB | Zigbee factory 数据 | 由 Zigbee 栈管理 |
| `coredump` | `0x351000` | 64 KiB | 崩溃转储 | 仅崩溃时 |
| `runtime_nvs` | `0x361000` | 64 KiB | 56 个调度任务的防重复/重试状态 | 状态实际变化且执行安全需要时 |

Stage 5 在 `coredump` 后增加 `ir_data`（`0x361000`，64 KiB），`runtime_nvs` 随后位于 `0x371000`。0.10.0 命名变体继续追加 `sensor_nvs`（`0x381000`）和 `cloud_nvs`（`0x391000`），均为 64 KiB；所有 Core 分区地址保持不变。最后一个分区结束于 `0x3A1000`，至 8 MiB Flash 末尾尚有约 4.37 MiB 未分配空间。这里的“未分配”是分区表计算结果，不代表 NVS 可用条目或运行时余量已经过硬件测量。

当前分区表不是 OTA 布局：只有一个 `factory` 应用，且没有 `otadata`、`ota_0`、`ota_1`。剩余 Flash 足以继续评估双 OTA 方案，但启用 OTA 必须重新设计分区并做升级/回滚和数据保留测试；本次没有冒险更改已发布分区地址。

## 数据规模与写入策略

| 数据 | 估计有效载荷上限 | 修复后的策略 |
|---|---:|---|
| 56 条定时配置 | 约 672 B（不含 NVS 元数据） | 仅页面提交的内容与 RAM 配置不同时提交一次 |
| 管理账户 | 用户名 33 B、salt 16 B、hash 32 B | 相同用户名/密码不重新生成 salt 或写入 |
| Wi-Fi 配置 | SSID 33 B、密码 65 B | 相同配置跳过；清空已空配置也跳过 |
| Zigbee 应用设备表 | 8 个固定快照，约 1 KiB 量级 | 内容未变化时跳过；检测到损坏时阻止自动覆盖并报警 |
| 单个调度执行记录 | 一个带版本、索引和校验的固定记录，约数十 B | 仅写对应任务；先持久化尝试标记再发送 OFF；瞬态命令 ID 只留 RAM |
| 调度操作记录 | 最近 8 条 | 固定 RAM 环形缓冲；不再写 Flash |
| 红外学习码 | 每槽最多 1024 个 16-bit duration，约 2.1 KiB；共 2 槽 | 仅确认保存且与已有数据不同时写入独立 NVS |
| 红外旧式 schedule | 小于 16 B | 内容变化时写；新统一调度配置位于主 NVS |

每项任务在一次成功执行中最多需要“新执行状态、发送前尝试标记、最终完成”三次小记录提交。失败时只按配置的有限重试次数增加记录；不会因日志或无限重试持续写 Flash。极端的 56 项同日全部执行仍需要硬件耐久/断电测试，不能仅凭结构估算声称寿命充足。

## NVS 初始化和故障策略

- `storage_manager_init()` 分别初始化主 NVS 与 `runtime_nvs`，绝不在错误路径自动擦除。
- `NO_FREE_PAGES` 被视为“无空闲页或分区被缩小”；`NEW_VERSION_FOUND` 被视为格式不兼容；空间不足、初始化失败、统计失败和数据校验失败分别记录原因。
- 主 NVS 不可用时，固件使用易失默认值，并允许临时随机 WPA2 配网 AP 启动；不会把默认值写回覆盖故障数据。
- `runtime_nvs` 不可用、写失败或记录校验失败时，自动调度停止，手动 Web、Wi-Fi、Zigbee 和红外控制不因此被擦除。
- 红外 NVS 不可用时只禁用红外子系统，照明与 Web 继续启动。
- Web `/api/diagnostics/resources` 显示每个 NVS 分区的 initialized、used/free/total entries、namespace count、最近错误以及写入/跳过/失败/损坏计数。`nvs_get_stats()` 的真实条目数必须在设备运行后采集。
- 破坏性恢复出厂设置擦除主/运行/红外/传感器/云 NVS 和 Zigbee 网络分区，但保留 coredump；它只能从管理员认证接口发起并要求精确确认文本。命令行 `erase-flash` 仍是破坏性操作，不属于自动恢复路径。

## 写入调用清单

源码级检查覆盖 `nvs_set_*`、`nvs_commit`、`nvs_erase_key`、`nvs_flash_erase*`、`esp_partition_write/erase_range`、`fopen` 和 `fwrite`：

- `settings.c`：Wi-Fi、设备名、管理员凭据、定时配置。
- `zigbee_manager.c`：`devices_v2` 设备/映射快照。
- `scheduler.c`：`runtime_nvs` 中的单项 `exNN` 记录。
- `backup_restore.c`：仅在管理员确认导入时向 `runtime_nvs/backup_tx/rollback` 写入一次约 5 KiB 的导入前快照；成功提交或成功回滚后删除。它用于跨主 NVS/IR NVS 的断电恢复，不是持续日志。
- `ir_storage.c`：两个红外码与旧式红外 schedule。

配置导入不会写入或擦除 Zigbee 分区、Wi-Fi/账户 key、RainMaker 身份或整个 NVS。导入前 JSON 全量验证；事务中断时在 `settings_init()` 之前恢复旧任务和 IR 码。真实拔电测试仍为 NOT TESTED。
- `storage_manager.c`：仅用户明确确认的恢复出厂设置，以及恢复标记。
- 没有应用文件系统写入、持久化调试日志或一般路径中的 `esp_partition_write()`。

ESP-IDF Wi-Fi、ZBOSS 和 coredump 组件内部仍会按其设计写各自分区；这些第三方组件的磨损行为不由本项目源码清单完全覆盖。

## 迁移与破坏性风险

- 旧 `exec_v1` 仅在新的逐项记录不存在时读取并迁移；成功或失败都保留旧 blob，便于回退和诊断。
- 旧 `log_v1` 可在首次运行时读入 RAM 供诊断，但从此不再更新，也不自动删除。
- 定时配置的 v2→v3 迁移失败时继续使用 RAM 中的有效旧数据并保留旧 NVS；下次启动可重试。
- 新增分区表不会移动既有分区，但首次部署仍必须烧入匹配 Stage 的 partition table。使用错误分区表、缩小已有 NVS、切换到未来 OTA 布局或执行 `erase-flash` 均具有破坏性，必须先备份和做迁移演练。
- 尚未在真实 ESP32-C6 上完成 NVS 满、写入时断电、Flash 磨损和损坏分区恢复测试，状态保持 `NOT TESTED`。
