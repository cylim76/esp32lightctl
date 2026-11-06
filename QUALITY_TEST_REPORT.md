# 代码质量与自动测试报告

日期：2026-11-04
版本：`0.11.0-stageN` / `0.11.0-{core,core_sensor,core_cloud,full}`
工具链：ESP-IDF 5.5.1、RISC-V GCC 14.2.0、esp-zigbee-lib 1.6.8、esp-zboss-lib 1.6.4。

## 交付结论

本轮源码检查、Web 契约检查、时间模型、JavaScript 解析、生产 C 测试工程构建、Stage 1～5 和 CORE/CORE_SENSOR/CORE_CLOUD/FULL 固件构建，以及九个构建的 `size` / `size-components` 均已实际执行并通过。没有把这些软件结果标记成硬件通过；Flash 断电、真实 NVS 满、无线掉线、RMT/RTC、长期磨损和无人值守稳定性仍为 `NOT TESTED`。

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

首轮 QEMU 确实暴露了结构体填充比较和测试栈问题；修复并加入内存不足注入后，从干净启动再次运行；本轮再加入备份 SHA-256/篡改检测，最终输出 `PHASE_A_C_TEST SUMMARY PASS: 16 tests`，随后保持运行，无 panic 后才人工终止 QEMU。

## 固件回归与资源

| Stage | 构建 | `.bin` | Flash Code | 静态 DIRAM | SHA-256 |
|---|---|---:|---:|---:|---|
| 1 | PASS | 1,076,832 B | 949,522 B | 160,376 B | `165131e2aec1a9bc8277211664c4a24aa032d95d9d9beb8eee3057ec9894a825` |
| 2 | PASS | 1,438,288 B | 1,302,730 B | 192,414 B | `6330952ff8d13d4ba11faee3e0aa169831429aae77ac4ce87cc5879a0eab86ef` |
| 3 | PASS | 1,441,648 B | 1,306,092 B | 192,414 B | `07ee15fba47ed29366dbb9bcd0b28368a15e40da7fdb63c07acc38e263c0922f` |
| 4 | PASS | 1,457,808 B | 1,322,254 B | 206,766 B | `28e95f312fc5e5ad576ab4b64894bc9cd89da42023d37d6a999af34663a5e00d` |
| 5 | PASS | 1,494,288 B | 1,354,430 B | 215,918 B | `b6576acc6164919829112ade6b89f1c72b82eb2227c1fbb4cdb426cdac78adb5` |

以上是构建期静态数据。运行时 Free Heap、Minimum Free Heap、Largest Free Block、各任务 Stack High Water Mark 和关键内存事件已在 Web 诊断实现，但数值必须在真实 ESP32-C6 负载下采集。

四种命名变体也已分别构建和统计：CORE / CORE_SENSOR / CORE_CLOUD / FULL 的 BIN 分别为 1,494,272 / 1,495,168 / 1,494,800 / 1,495,552 B；对应 SHA-256 与 Flash/DIRAM 明细见 `MEMORY_RESOURCE_REPORT.md`。CORE 的构建图确认不含 BH1750/SHT31/RainMaker 组件；扩展硬件和真正 RainMaker 传输仍为 `NOT TESTED / NOT IMPLEMENTED`。

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
- 管理页使用表单登录和 RAM 会话令牌；登录请求仍是局域网明文 HTTP，量产环境的 TLS、Flash Encryption/NVS Encryption 需另行设计。

不存在自动破坏性迁移；存在且明确记录的破坏性操作只有用户确认恢复出厂设置、手工 `erase-flash` 和未来分区重排。

## 0.11.0 配置备份与恢复增量报告

### 确定问题及修复

| 问题 | 风险 | 修改 |
|---|---|---|
| 缺少可移机的配置导出 | 开发板损坏后需人工重建大量定时和 IR 码 | 新增版本化 JSON 导出、两组完整 IR OFF 码和 56 条定时 |
| 跨主 NVS 与 `ir_data` 无原子事务 | 导入中断电可能形成新旧混合配置 | 提交前在 `runtime_nvs/backup_tx` 持久化旧快照，启动时先于 settings/IR 加载回滚 |
| 直接上传即可覆盖配置 | 格式/范围错误可能写到一半 | 分为 validate/token/明确 commit；验证阶段零写入，token 2 分钟失效 |
| 备份可能误含身份/密钥 | 文件泄露扩大影响或错误恢复 Zigbee | 明确排除 Wi-Fi/管理员密码、RainMaker 凭据、Zigbee 密钥/配对/映射，并在新板报告重新配对 |
| 大文件/频繁上传可能消耗堆 | 512 KiB SRAM 下造成碎片或拒绝服务 | 48 KiB 上限、10 秒限流、按需堆分配、候选 2 分钟自动释放、关键内存事件 |
| 导入 IR 时与学习/发射并行 | 状态缓存陈旧或读写竞争 | IR busy 时拒绝提交，成功或回滚后刷新 IR metadata |

### 实际自动检查

- `tests/test_backup_contract.py`：PASS。
- `tests/test_backup_restore.py`：PASS；覆盖正常导入、严格拒绝尾随 JSON、格式/版本/校验/超限失败、模拟掉电回滚、重复导入与新板恢复。
- `tests/test_backup_restore.py`：正常导入、结构错误、版本不兼容、校验失败、超大文件、模拟断电回滚、重复导入、新板恢复全部 PASS。
- Web 内联 JavaScript：解析 PASS。
- C SHA-256 已知向量、篡改、非法摘要测试：ESP32-C3 QEMU 实际执行 PASS；完整 C 测试 `16/16 PASS`。
- ESP32-C6 C 测试镜像：BUILD PASS，126,992 B，SHA-256 `e50367269418bd87140b57a407e1ada78e0f43ec8a51f8ba178d4bb4d1d5d451`。
- Stage 1～5、CORE、CORE_SENSOR、CORE_CLOUD、FULL 的 build/size/size-components：全部 PASS；具体大小与 SHA-256 见 `BUILD.md` 和 `MEMORY_RESOURCE_REPORT.md`。
- `git diff --check`：PASS（只有 Windows CRLF 转换提示）。

### 未自动或未实物验证

- 浏览器与真实 ESP32-C6 间的下载/上传行为：NOT TESTED；表单登录、会话令牌和旧哈希迁移正在目标板复测。
- 主 NVS/IR NVS 每个写入窗口的真实拔电恢复：NOT TESTED。
- 最大 48 KiB JSON 在 C6 上的实际堆峰值和碎片恢复：NOT TESTED。
- 导入两组真实 LG 码后的空调响应、新板 Aqara 重新配对/端点确认：NOT TESTED。
- 扩展模块当前没有用户可调参数；备份只携带独立 schema 和空设置对象。未来增加参数时必须同时增加版本迁移和 round-trip 测试。

## P1/P2 增量结果（2026-11-04）

- Web 扩展管理契约：PASS。验证三个独立 NVS key、未编译模块拒绝、认证写 API、页面状态语义和 Core 隔离。
- Web 内联 JavaScript 解析：PASS。
- BH1750 协议 C 测试：换算、范围拒绝、中位滤波 PASS。
- SHT31 协议 C 测试：已知 CRC、温湿度解码、损坏 CRC 拒绝 PASS。
- QEMU：18/18 PASS；ESP32-C6 测试镜像 BUILD PASS。
- `CORE_SENSOR` 真实 `idf.py build`：PASS。
- 真实 BH1750/SHT31、共享 I²C、运行时反复启停、长期 Heap 与数值精度：NOT TESTED。
- 本节记录的是 P1/P2 当时状态；扩展设置备份已在后续 P4 完成，最终结论见下方 P4 结果。

## P3 RainMaker Classic 软件结果（2026-11-04）

- 官方 RainMaker Classic 1.17.0 和 rmaker_common 1.9.0 固定依赖：IMPLEMENTED / BUILD PASS。
- 三路照明 ON/OFF、全部灯 OFF、LG 空调 1/2/组明确 OFF 的控制策略 C 测试：SOFTWARE TEST PASS。
- 云端缓存初始化值忽略、OFF 触发器释放、并发命令拒绝、空调 ON 拒绝：SOFTWARE TEST PASS。
- 灯状态同步仅接受在线、已确认映射、未过期且未回读超时的 Zigbee 端点缓存：源码契约 PASS；真实 Zigbee/App NOT TESTED。
- CORE_CLOUD/FULL 真实 `build`、`size`、`size-components`：BUILD PASS；详见 `BUILD.md` 与 `MEMORY_RESOURCE_REPORT.md`。
- Claiming、用户账户绑定、TLS/MQTT、云端断线重连、手机 App 控制、受控重启后的禁用：CLOUD/HARDWARE NOT TESTED。
- RainMaker 停用不会尝试不安全的热销毁；配置先持久化，API 明确返回 `restart_required`。受控重启行为仍需目标板验证。

本功能没有破坏性数据迁移，不会擦除任何整个分区。SHA-256 是完整性检查而非数字签名；管理登录和备份传输没有 TLS，只能在可信本地网络或安全通道使用。

## P4 扩展配置备份与事务恢复结果（2026-11-04）

- BH1750 启用状态、采样周期、夜间时段、Lux 阈值、连续异常次数和五场景标定值：IMPLEMENTED / SOFTWARE TEST PASS。
- SHT31 启用状态与采样周期：IMPLEMENTED / SOFTWARE TEST PASS。
- RainMaker 非敏感启用偏好：IMPLEMENTED / SOFTWARE TEST PASS；Claiming 凭据、证书和密钥继续排除。
- 旧版空 `non_sensitive_settings`：兼容导入并跳过，不覆盖现有扩展设置。
- 未编译扩展：导入时跳过并报告，保留独立 NVS，支持 FULL→CORE→FULL。
- 扩展设置与 56 条任务、两组 IR 码共同进入验证、确认、提交和回滚事务；旧版事务日志可恢复。
- 正常导入、尾随 JSON、结构错误、版本不兼容、校验失败、超大文件、模拟断电回滚、重复导入、新板恢复、旧空占位、扩展范围错误：HOST SOFTWARE TEST PASS。
- CORE、CORE_SENSOR、CORE_CLOUD、FULL 增量 `idf.py build`：BUILD PASS；大小和 SHA-256 见 `BUILD.md`。
- 完整 `check-all.bat`：PASS；Stage 1～5 与四种固件均完成 `build`、`size`、`size-components`，并通过 ESP32-C6 C 测试镜像构建、Web JavaScript、存储/变体/扩展/RainMaker/备份契约和调度模拟。
- 本轮 ESP32-C6 C 测试镜像：BUILD PASS，129,936 B，SHA-256 `e116b28abf2a54619288b057d4566e35f57435b0e873405ed51b7c0aa699f608`；尚未在 C6 实板运行。
- 浏览器到真实 ESP32 的导出/导入、真实 Flash 写入中断电以及硬件回滚：NOT TESTED。

本轮额外修复了禁用传感器配置导出：未启动采样任务时从 `sensor_nvs` 读取配置；读取失败会拒绝导出/事务快照，不再生成无效零值备份。

## P5 安全回退软件验收（2026-11-04）

- 四种变体共用 `partitions_stage5.csv`，核心及扩展分区顺序/地址不因变体改变：SOURCE TEST PASS。
- 未编译扩展在应用备份时先跳过，不初始化、不打开、更不擦除其 NVS 分区：SOURCE TEST PASS。
- FULL→CORE 保留 Wi-Fi/管理员、Zigbee、IR、运行状态、56 条任务、Sensor/Cloud 数据；CORE 更新核心配置后再切回 FULL，扩展数据重新可见：HOST SIMULATED PASS。
- `ROLLBACK.md` 要求先备份完整 8 MiB 及关键分区，并明确禁止 `erase-flash`：DOCUMENT CONTRACT PASS。
- 真实开发板 FULL→CORE→FULL、断电和失败回退：HARDWARE NOT TESTED；因此没有创建 stable Git Tag。
## P6 文档与交付契约（2026-11-04）

- 14 份必交文档存在且可由 README 到达：`DELIVERY_DOCS PASS`。
- 模块、传感器、RainMaker 和遗漏审计必须显式区分 `IMPLEMENTED`、`BUILD PASS`、`SOFTWARE TEST PASS` 与 `NOT TESTED`；自动检查已覆盖。
- `HARDWARE_TEST_RECORD.md` 的每项状态只允许 `PASS`、`FAIL` 或 `NOT TESTED`；本轮 70 项仍全部为 `NOT TESTED`，没有用编译或 QEMU 结果冒充实物通过。
- 四种正式固件变体和稳定 Tag 门禁均有文档契约；由于尚无实机完整验收，本轮不创建 `core-stable-v1.0`。
- 本节只验证文档、状态和工作流契约；P4 的 Stage 1～5、四变体、C6 测试镜像真实构建结果仍适用于当前固件源码，P5/P6 未修改固件源码。

## Stage 1 首次实机缺陷修复（2026-11-06）

确定问题与修复：

- `backup_restore.c` 在启动恢复路径把大型原生备份载荷放在 `app_main` 栈上，实机可触发栈故障；改为限额堆分配，并增加源码契约测试。
- 每个 HTTP Basic API 请求都在单核 C6 上执行 120,000 次 PBKDF2，`httpd` 长时间占用 CPU 并触发任务看门狗；改为一次表单登录、随机 RAM 会话令牌和 12 小时滑动过期。旧 120,000 次凭据继续兼容，首次成功登录后异步迁移到 8,192 次，派生循环主动让出 CPU。
- `time_manager.c` 在 critical section 内调用本地时间换算，NTP 成功后触发 Newlib 锁异常；所有相关换算移到临界区外。DS3231 离线时不再尝试 I2C 写入，只记录 `ESP_ERR_NOT_FOUND`。
- 临时配网热点改为每次随机 10 位纯数字 WPA2 密码，仅串口显示且配网完成关闭。
- `flash.bat` 和 `monitor.bat` 会优先自动加载相邻的 ESP-IDF 5.5.1 环境，减少命令提示符未执行 `export.bat` 的失败。

实际自动检查：`check-all.bat` PASS；包含存储静态审计、备份恢复、认证、时间管理、交付文档、扩展/RainMaker 契约、调度模拟、网页 JavaScript、C6 C 测试镜像构建，以及 Stage 1～5 和四种正式固件的真实 build/size/size-components。PBKDF2 生产 C 实现使用标准 SHA-256 向量测试。详细体积和 SHA-256 见 `BUILD.md`。

实际硬件检查：Stage 1 烧录、Wi-Fi STA、DHCP、NTP、NVS 配置保留和约 15 分钟空闲资源趋势通过；NTP 修复后未再次崩溃，Free Heap 稳定约 283 KB。mDNS 在固件端发布成功但当前客户端无法解析，记录 FAIL 并保留 IPv4 回退。管理员首次登录迁移、新版数字配网密码以及断线重连仍需实机复测；Zigbee、RTC、红外、传感器和 RainMaker 不因本轮 Stage 1 结果改为硬件 PASS。
