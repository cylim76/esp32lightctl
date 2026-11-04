# 最新代码遗漏审计

审查基准：`main` / `53c38486b79a2e24924223439537d34f2da89eba`。本表区分源码存在、可编译、软件测试和真实硬件/云端验收；前两者不能替代后两者。

| 需求 | 现有代码 | 完成状态 | 需要修改的文件 | 验收测试 |
|---|---|---|---|---|
| P0 ESP-IDF CI 环境 | 所有容器 Job 显式加载 `/opt/esp/idf/export.sh` | IMPLEMENTED / GITHUB CI PASS (`41742751482`) | `.github/workflows/quality.yml`、`tests/test_ci_workflow.py` | Stage 1～5、四变体及 C6 测试镜像的远端 Job 全部 PASS |
| P0 QEMU 数量 | 工作流从测试输出读取 PASS/FAIL 汇总，不再硬编码数量 | IMPLEMENTED / GITHUB CI PASS (`41742751482`) | `.github/workflows/quality.yml` | 存在 PASS 汇总、无 FAIL，QEMU Job PASS |
| 四种编译变体 | CORE、CORE_SENSOR、CORE_CLOUD、FULL 已隔离构建 | IMPLEMENTED / BUILD PASS | 回归 `CMakeLists.txt`、Kconfig、变体 defaults | 四种独立 build/size/size-components PASS |
| Core 不依赖扩展 | 扩展为独立组件，CORE 不加入组件目录 | IMPLEMENTED / SOFTWARE TEST PASS | 持续回归 `tests/test_variant_contract.py` | 删除/禁用扩展后 CORE 构建；目标板核心功能 NOT TESTED |
| Web 扩展管理 | 三模块独立状态、认证 API、页面和未编译保护已完成 | IMPLEMENTED / SOFTWARE TEST PASS / BUILD PASS | `main/extension_manager.*`、`main/web_server.c`、`main/web/index.html` | 实机启停、重启恢复和 Heap 无泄漏仍 NOT TESTED |
| 扩展启用状态持久化 | 独立 key 写入 `sensor_nvs/cloud_nvs`，未变化跳过 | IMPLEMENTED / SOFTWARE TEST PASS | `main/extension_manager.c`、存储诊断 | 真实掉电与损坏分区恢复 NOT TESTED |
| BH1750 驱动 | 真实模式设置、读取、换算、中位滤波、重试和 20 点 RAM 历史 | IMPLEMENTED / SOFTWARE TEST PASS / BUILD PASS | `extensions/bh1750/*`、Web、测试 | 数值精度与实物掉线恢复 NOT TESTED |
| BH1750 标定与夜间异常 | 五场景标定、时段/阈值/连续次数和整体异常已实现 | IMPLEMENTED / BUILD PASS | BH1750 配置、Web、NVS；备份在 P4 完成 | 实际办公室阈值校准 NOT TESTED |
| SHT31 驱动 | 真实 `0x2400` 测量、双 CRC、趋势、重试和 20 点 RAM 历史 | IMPLEMENTED / SOFTWARE TEST PASS / BUILD PASS | `extensions/sht31/*`、Web、测试 | 精度、拔插及总线并发 NOT TESTED |
| RainMaker Classic | 固定 SDK 1.17.0，已实现 Node、self-claim、用户映射、MQTT 事件与重连所需 SDK 路径 | IMPLEMENTED / SOFTWARE TEST PASS / BUILD PASS / CLOUD NOT TESTED | `extensions/rainmaker/*`、组件依赖、Web 映射入口与测试 | 真实 Claiming/TLS/MQTT/App 控制日志；本地 Core 断云回归 |
| Cloud Control ON/OFF | 三路灯 ON/OFF、全部 OFF、三种 IR OFF 均经统一 Device Control Service | IMPLEMENTED / SOFTWARE TEST PASS / BUILD PASS / HARDWARE NOT TESTED | `device_control.*`、RainMaker 适配、Zigbee 状态同步 | 新鲜 Zigbee 反馈同步；IR 只报已发送；真实设备/App 待测 |
| 扩展配置备份 | v1 已包含启用状态、BH1750 配置/标定、SHT31 周期、RainMaker 非敏感偏好 | IMPLEMENTED / SOFTWARE TEST PASS / BUILD PASS / HARDWARE NOT TESTED | `backup_restore.*`、扩展配置版本/迁移测试 | 旧空占位兼容；扩展设置 round-trip；秘密排除；v1/v2 掉电回滚 |
| FULL→CORE→FULL | 分区地址稳定、扩展缺失跳过且不擦除，具备数据保留模拟测试 | SOFTWARE TEST PASS / HARDWARE NOT TESTED | `test_variant_rollback.py`、`ROLLBACK.md`、硬件记录 | 实板核心/扩展数据保留、断电和失败回退演练 |
| Heap/栈/Flash | 静态 size 与核心诊断已存在 | PARTIALLY IMPLEMENTED | 传感器/RainMaker事件监测和报告 | 启停、TLS、采样峰值；泄漏与长期趋势硬件测试 |
| MODULE_MANAGEMENT.md | 已与运行时 API、NVS key 和限制同步 | IMPLEMENTED | 持续随接口更新 | 文档契约检查 |
| Sensor/RainMaker 文档 | Sensor 两份及 RainMaker 设计/设置/测试计划已完成 | IMPLEMENTED | `SENSOR_WIRING.md`、`ENVIRONMENT_MONITORING.md`、`RAINMAKER_*.md` | 每项标明 IMPLEMENTED/BUILD/SOFTWARE/HARDWARE 状态 |
| 稳定 Git Tag | 未创建，符合“硬件通过后再创建” | NOT TESTED / 正确保留 | 硬件验收后才操作 | CORE 实机完整验收通过后创建不可移动标签 |

## 实施边界

1. 不重写已经完成的 RTC、56 条任务、备份事务、LG IR 和核心控制。
2. 每阶段独立提交；本地 PASS 与 GitHub CI PASS 分开记录。
3. 传感器软件测试使用可注入 I²C 传输层；没有目标硬件日志时保持 `NOT TESTED`。
4. RainMaker 源码和构建可标记 IMPLEMENTED/BUILD PASS；真实 TLS/MQTT、账户绑定和 App 控制在取得云端日志前必须保持 CLOUD/HARDWARE NOT TESTED。

## 分阶段交付记录

| 阶段 | Git 提交 | 自动验收状态 | 仍需实物/云端验证 |
|---|---|---|---|
| P0 CI 修复 | `685abea96e434c966687f9279f0d514f9c937889` | GitHub Actions `41742751482` PASS | 无（仅 CI 基础设施） |
| P1/P2 运行时管理与传感器 | `c99c2c82793614e4cc63ebb0b30bdaa7b4df2a49` | GitHub Actions `41751177639` PASS | BH1750/SHT31 精度、拔插、I²C 共存、启停 Heap |
| P3 RainMaker | `1d64b595da2b7d10a7f50305b07283d125fb029d` | GitHub Actions `41755616056` PASS | Claiming、账户绑定、TLS/MQTT、App 控制和断云隔离 |
| P4 扩展配置备份 | `5f51fcb1a5a0aca3c03033d677f1888918fe47af` | 本地完整 `check-all.bat` PASS；GitHub Actions `41786371818` PASS | 实板 NVS、断电回滚、新板恢复 |
| P5 安全回退 | `0dd5c974f482e4dcbbe9b9d218da2dfee19ccccd` | 数据保留模拟和变体契约 PASS；GitHub Actions `41786731060` PASS | FULL→CORE→FULL 实板回退 |
| P6 文档验收 | 当前提交 | `test_delivery_docs.py` 检查文档、状态和 Tag 门禁 | `HARDWARE_TEST_RECORD.md` 中全部项目 |

最终 GitHub Actions 只有在远端工作流全部结束且结论为 `success` 后才报告 CI PASS；运行中状态不会被写成 PASS。
