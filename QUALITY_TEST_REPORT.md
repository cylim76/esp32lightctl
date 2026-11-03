# 代码质量与自动测试报告

日期：2026-11-03  
版本：`0.9.0-stageN`  
工具链：ESP-IDF 5.5.1、RISC-V GCC 14.2.0、esp-zigbee-lib 1.6.8、esp-zboss-lib 1.6.4。

## 交付结论

本轮源码检查、Web 契约检查、时间模型、JavaScript 解析、15 项生产 C 逻辑 QEMU 测试、ESP32-C6 测试镜像构建、Stage 1～5 固件构建以及五个 Stage 的 `size` / `size-components` 均已实际执行并通过。没有把这些软件结果标记成硬件通过；Flash 断电、真实 NVS 满、无线掉线、RMT/RTC、长期磨损和无人值守稳定性仍为 `NOT TESTED`。

## 确定问题与修复

| 确定问题 | 影响 | 修改 |
|---|---|---|
| 默认 NVS 初始化异常时自动 `nvs_flash_erase()` | 可无确认删除全部用户配置 | 集中到 `storage_manager`；错误时只诊断并降级，永不自动擦除 |
| 红外 NVS 初始化异常时自动擦除 | 可删除已学习 LG OFF 码 | 改为非破坏性禁用红外，照明/Web 继续运行 |
| `exec_v1` 每次整体写 56 项 | 写放大、失败范围大 | 独立 64 KiB `runtime_nvs`，`ex00`～`ex55` 单项记录、版本和校验 |
| `log_v1` 每条日志整体写 Flash | 长期无人值守时持续磨损 | 最近 8 条固定 RAM 环形缓冲；旧日志只读不再写 |
| 配置保存未统一判断“实际变化” | 相同设置重复提交 NVS | Wi-Fi、设备名、账户、定时、红外码、设备表均增加 unchanged 跳过 |
| v2 schedule 迁移时源/目标可能指向同一 RAM 数组 | 成功写入后 RAM 任务可能被清零至下次重启 | 写入前复制固定快照，迁移失败保留旧数据并下次重试 |
| 旧执行状态迁移中断后只要存在一项新记录就跳过余项 | 断电后部分任务失去防重复状态 | 迁移改为逐项可续传，只填充缺失记录并保留 `exec_v1` |
| Zigbee `devices_v2` 损坏时清空 RAM 后可能被自动覆盖 | 诊断证据和原设备映射可能丢失 | 标记损坏并阻止自动覆盖，等待显式恢复 |
| 执行状态用结构体 `memcmp` 比较并把填充字节纳入记录 | 可能误判变化、增加 Flash 写；QEMU 56 项测试真实发现 | 改为逐字段比较和零填充的确定性序列化 |
| 测试自身把两个大型 56 项 store 放在 main task 栈 | QEMU 首轮出现栈破坏，干扰故障结论 | 测试 store 移到 BSS；测试结束后阻塞而非返回 |

## 实际测试结果

| 检查 | 结果 | 证据范围 |
|---|---|---|
| `tests/static_storage_check.py` | PASS | 清点 30 个写入/擦除调用；禁止 storage manager 外的自动 NVS erase、旧整体 exec/log 写、原始分区写 |
| `tests/test_web_storage_contract.py` | 6/6 PASS | 认证、精确擦除确认、NVS stats/错误计数、页面容量与确认 UI |
| `tests/simulate_time_scheduler.py` | 14 个场景 SIMULATED PASS | RTC/NTP/离线、时间跳变、补执行、有限重试、IR 隔离、56 项与 v2 迁移 |
| `tests/check_web_js.js` | PASS | 嵌入页面内联 JavaScript 解析 |
| ESP32-C3 QEMU 生产 C 测试 | 15/15 SIMULATED PASS | Zigbee/命令状态 8 项 + 调度持久化及故障注入 7 项 |
| ESP32-C6 C 测试镜像 | BUILD PASS | 117,680 B；SHA-256 `3a4151fb1328aa651eacbaa053fad780da4a598962df25bdb8f564cb952ea111` |
| `git diff --check` | PASS | 无 whitespace error；仅 Windows CRLF 提示 |
| ESP-IDF 编译诊断 | PASS | 五个 Stage 在 `-Wall -Werror=all` 工程配置下构建通过 |
| `idf.py size` / `size-components` | PASS | Stage 1～5 两类命令退出 0 |

本机没有安装 `cppcheck`、`clang` 或 `clang-tidy`，因此未伪报这些工具通过。自定义存储策略静态检查、GCC 警告即错误和接口契约检查已纳入 `check-all.bat` 与 GitHub Actions。

## C 故障测试明细

QEMU 测试直接编译生产文件 `zigbee_state_cache.c`、`device_state.c` 和 `scheduler_persistence.c`：

1. 不同 IEEE 设备、相同 endpoint 的状态隔离。
2. 过期 OFF 状态失效。
3. 回读 pending/timeout 不接受旧 OFF。
4. 重新配对 generation 失效。
5. 超时后只有新反馈才能恢复有效。
6. Zigbee 短地址复用隔离。
7. 命令 PENDING→SENT→CONFIRMED 生命周期。
8. 命令环形缓冲不覆盖未完成项。
9. 相同执行状态第二次保存跳过写入。
10. 注入 `ESP_ERR_NVS_NOT_ENOUGH_SPACE` 后保留最后提交状态。
11. 注入写入中断/失败后重启读取最后完整状态。
12. 校验损坏记录被拒绝。
13. 56 条记录全部保存并模拟重启恢复。
14. 1000 次相同更新只发生 1 次后端写入。
15. 后端返回 `ESP_ERR_NO_MEM` 时错误向上传递，重启仍读取最后完整状态。

首轮 QEMU 确实暴露了结构体填充比较和测试栈问题；修复并加入内存不足注入后，从干净启动再次运行，输出 `PHASE_A_C_TEST SUMMARY PASS: 15 tests`，随后保持运行，无 panic 后才人工终止 QEMU。

## 固件回归与资源

| Stage | 构建 | `.bin` | Flash Code | 静态 DIRAM | SHA-256 |
|---|---|---:|---:|---:|---|
| 1 | PASS | 1,076,416 B | 949,114 B | 160,356 B | `ccc64d3b6ad5963ce2d9756a79a50a2261fb7a4658e45b98fb28dafc6627b866` |
| 2 | PASS | 1,437,856 B | 1,302,316 B | 192,386 B | `bfcadad4dda22648f64e094bbb2c7d6c4b61941aae03994f0d41498d2f6aa2cc` |
| 3 | PASS | 1,441,232 B | 1,305,682 B | 192,386 B | `dc55fd7f7926ae131b77914b4040d2389c53e58ea525819d61e45db02c773190` |
| 4 | PASS | 1,457,392 B | 1,321,844 B | 206,738 B | `3639c525a9d005e1830a96bc115ad9f5f48d37960b0b344ffd7a8a83e42d9d7a` |
| 5 | PASS | 1,493,872 B | 1,354,022 B | 215,902 B | `35d32c750c6965861c3ed2ecb3ad4bce027784c716f58cdd46d5e6b36659ec8e` |

以上是构建期静态数据。运行时 Free Heap、Minimum Free Heap、Largest Free Block、各任务 Stack High Water Mark 和关键内存事件已在 Web 诊断实现，但数值必须在真实 ESP32-C6 负载下采集。

## 故障要求覆盖状态

| 故障 | 自动结果 | 真实硬件 |
|---|---|---|
| NVS 空间不足 | C 故障注入 PASS | NOT TESTED |
| NVS 一般写失败 | C 故障注入 PASS，自动调度 fail-safe 停止 | NOT TESTED |
| 写入过程中断电 | 旧完整记录恢复模型 PASS | 真实断电 NOT TESTED |
| 配置版本升级/迁移 | v2 host 模型 PASS；源码策略检查 PASS；迁移可续传 | 真实旧镜像升级 NOT TESTED |
| 56 条任务 | C 保存/重启恢复 PASS；host 容量 PASS | 同分钟负载 NOT TESTED |
| 高频触发和重试 | 1000 次 unchanged 抑制 PASS；有限重试模型 PASS | NOT TESTED |
| 长期日志存储 | 源码确认日志仅固定 RAM、无 Flash 写路径 | 7 天磨损/增长 NOT TESTED |
| Wi-Fi 掉线 | 有效时间离线模型 PASS | RF/重连 NOT TESTED |
| Zigbee 掉线/超时 | 状态缓存/有限重试 C 测试 PASS | QBKG25LM 实物 NOT TESTED |
| RainMaker 掉线 | Stage 6 尚未实现，NOT AVAILABLE | NOT TESTED |
| 红外失败 | 设备独立执行模型 PASS；源码异步隔离已编译 | RMT/空调 NOT TESTED |
| RTC 失效/时间跳变 | OSF、时间有效性和防重复模型 PASS | DS3231 NOT TESTED |
| 内存分配失败 | C 后端 `ESP_ERR_NO_MEM` 注入 PASS，最后已提交状态保持完整 | 真实堆耗尽/硬件 NOT TESTED |
| 系统重启恢复 | 逐项执行状态重启 C 测试 PASS | 真实掉电 NOT TESTED |

## 修改文件

- 存储安全：`main/storage_manager.[ch]`、`main/main.c`、`main/settings.c`、`main/ir_storage.c`、`main/zigbee_manager.c`、`main/wifi_manager.c`。
- 调度持久化：`main/scheduler_persistence.[ch]`、`main/scheduler.c`、`main/app_config.h`、`main/CMakeLists.txt`。
- 分区：`partitions.csv`、`partitions_stage5.csv`。
- Web：`main/web_server.c`、`main/web/index.html`。
- 测试/CI：`tests/phase_a_app/**`、`tests/static_storage_check.py`、`tests/test_web_storage_contract.py`、`check-all.bat`、`.github/workflows/quality.yml`。
- 文档/版本：`README.md`、`BUILD.md`、`TEST_PLAN.md`、`HARDWARE_TEST_RECORD.md`、`MEMORY_RESOURCE_REPORT.md`、`STORAGE_AUDIT.md`、本报告、`CMakeLists.txt`、`main/stage_config.h`。

## 剩余风险

- 主 NVS 只有 24 KiB；Web 已能显示真实 entries，但设备尚未运行，不能声称长期余量充足。
- `runtime_nvs` 的实际磨损、掉电原子性和 Flash 芯片寿命仍需板上压力测试。
- Zigbee 栈、Wi-Fi 驱动和 coredump 的内部写入不完全受应用层写入计数覆盖。
- 用户确认的恢复出厂设置是多分区操作；若擦除过程中再次断电，可能形成部分擦除，只能通过串口/页面诊断和重新确认恢复，无法事务回滚。
- 当前仍是 factory-only 分区表，无双 OTA；启用 OTA 属于未来有迁移风险的分区变更。
- 管理页仍使用局域网 HTTP Basic，量产环境的 TLS、Flash Encryption/NVS Encryption 需另行设计。

不存在自动破坏性迁移；存在且明确记录的破坏性操作只有用户确认恢复出厂设置、手工 `erase-flash` 和未来分区重排。
