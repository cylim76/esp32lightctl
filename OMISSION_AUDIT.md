# 最新代码遗漏审计

审查基准：`main` / `53c38486b79a2e24924223439537d34f2da89eba`。本表区分源码存在、可编译、软件测试和真实硬件/云端验收；前两者不能替代后两者。

| 需求 | 现有代码 | 完成状态 | 需要修改的文件 | 验收测试 |
|---|---|---|---|---|
| P0 ESP-IDF CI 环境 | 容器 Job 直接运行 `idf.py`，未加载 `export.sh` | NOT IMPLEMENTED / CI FAIL | `.github/workflows/quality.yml`、`tests/test_ci_workflow.py` | Stage 1～5、四变体及 C6 测试镜像的远端 Job 全部 PASS |
| P0 QEMU 数量 | 实际 16 项，工作流硬编码 15 | NOT IMPLEMENTED / CI FAIL | `.github/workflows/quality.yml` | 不硬编码数量；存在 PASS 汇总、无 FAIL，QEMU Job PASS |
| 四种编译变体 | CORE、CORE_SENSOR、CORE_CLOUD、FULL 已隔离构建 | IMPLEMENTED / BUILD PASS | 回归 `CMakeLists.txt`、Kconfig、变体 defaults | 四种独立 build/size/size-components PASS |
| Core 不依赖扩展 | 扩展为独立组件，CORE 不加入组件目录 | IMPLEMENTED / SOFTWARE TEST PASS | 持续回归 `tests/test_variant_contract.py` | 删除/禁用扩展后 CORE 构建；目标板核心功能 NOT TESTED |
| Web 扩展管理 | 无页面、API、统一状态结构 | NOT IMPLEMENTED | `main/extension_manager.*`、`main/web_server.c`、`main/web/index.html` | 三模块独立启停、未编译按钮禁用、状态字段和认证 API 测试 |
| 扩展启用状态持久化 | 只有命名空间常量，未读写 `sensor_nvs/cloud_nvs` | NOT IMPLEMENTED | 扩展配置模块、存储管理、迁移测试 | 仅变更时写 NVS；重启恢复；损坏配置不影响 Core |
| BH1750 驱动 | 仅 I²C 地址探测 | PARTIALLY IMPLEMENTED | `extensions/bh1750/*`、Web、测试 | 命令/读取/换算/滤波/重试/20 点趋势的软件测试；实物 NOT TESTED |
| BH1750 标定与夜间异常 | 无配置、记录和检测 | NOT IMPLEMENTED | BH1750 配置、Web、NVS、备份 | 五种场景标定；阈值/时段/连续次数；只报告整体异常 |
| SHT31 驱动 | 仅 I²C 地址探测 | PARTIALLY IMPLEMENTED | `extensions/sht31/*`、Web、测试 | 测量命令、CRC、趋势、重试软件测试；实物 NOT TESTED |
| RainMaker Classic | 适配层返回 `ESP_ERR_NOT_SUPPORTED` | NOT IMPLEMENTED | `extensions/rainmaker/*`、组件依赖、独立示例与测试 | 真实 Claiming/TLS/MQTT/App 控制日志；本地 Core 断云回归 |
| Cloud Control ON/OFF | 统一服务当前主要按关闭目标调用 | PARTIALLY IMPLEMENTED | `device_control.*`、RainMaker 适配、Zigbee 状态同步 | 三路 ON/OFF、全部 OFF、三种 IR OFF；不虚报空调状态 |
| 扩展配置备份 | v1 只有空 `non_sensitive_settings` | PARTIALLY IMPLEMENTED | `backup_restore.*`、扩展配置版本/迁移测试 | v1 兼容；扩展设置 round-trip；秘密字段持续排除；掉电回滚 |
| FULL→CORE→FULL | 分区地址与回退流程已设计 | PARTIALLY IMPLEMENTED / NOT TESTED | 回归分区、`ROLLBACK.md`、硬件记录 | 核心数据保持；扩展数据保留；不得擦除；实物演练 |
| Heap/栈/Flash | 静态 size 与核心诊断已存在 | PARTIALLY IMPLEMENTED | 传感器/RainMaker事件监测和报告 | 启停、TLS、采样峰值；泄漏与长期趋势硬件测试 |
| MODULE_MANAGEMENT.md | 文件不存在 | NOT IMPLEMENTED | 新增文档 | 与实际 API、状态机和限制一致 |
| Sensor/RainMaker 文档 | 指定文档不存在 | NOT IMPLEMENTED | 新增五份文档并更新既有报告 | 每项标明 IMPLEMENTED/BUILD/SOFTWARE/HARDWARE 状态 |
| 稳定 Git Tag | 未创建，符合“硬件通过后再创建” | NOT TESTED / 正确保留 | 硬件验收后才操作 | CORE 实机完整验收通过后创建不可移动标签 |

## 实施边界

1. 不重写已经完成的 RTC、56 条任务、备份事务、LG IR 和核心控制。
2. 每阶段独立提交；本地 PASS 与 GitHub CI PASS 分开记录。
3. 传感器软件测试使用可注入 I²C 传输层；没有目标硬件日志时保持 `NOT TESTED`。
4. RainMaker 只有真实 SDK、真实 TLS/MQTT 和账户绑定链路跑通后才标记 IMPLEMENTED；兼容性受阻时记录证据并保持 NOT IMPLEMENTED。
