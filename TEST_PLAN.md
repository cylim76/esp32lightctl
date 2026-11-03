# 硬件验证计划

所有硬件结果统一填写到 `HARDWARE_TEST_RECORD.md`。只允许 `PASS`、`FAIL`、`NOT TESTED`；`idf.py build` 成功不能填写为硬件 PASS。

## 推荐顺序

1. Stage 1：只验证供电、串口、首次配网、Web、鉴权、NVS、Wi-Fi 重连、可选 RTC 和时间诊断。
2. Stage 2：确认 Stage 1 无回归后验证单芯片 Wi-Fi/Zigbee 共存、Coordinator 建网与 180 秒 Permit Join。
3. Stage 3：复位 QBKG25LM 并配对，只记录实际 Active Endpoints、输入/输出 Cluster、model 和状态报文，不控制市电负载。
4. 根据串口 `DIAG` 和实际逐路实验确定映射。三路端点未确认前禁止点击映射确认。
5. Stage 4：保存已验证映射后，分别测试左/中/右、实体按键反馈、全关、定时、掉线恢复、断电恢复与长稳。
6. Stage 5：先回归 Stage 4，再严格按 `IR_TEST_PLAN.md` 分别验证 RX 学习、TX 波形、单台空调、两台空调、定时和供电/RF 共存。

## Stage 1

- 首次启动显示随机 WPA2 配置热点和一次性密码。
- 手机连接 `192.168.4.1`，保存 Wi-Fi 与至少 12 位管理密码。
- 重启后 STA 获得 IP，页面需要 HTTP Basic 认证。
- 页面显示 RSSI、IP、运行时间、Free Heap、Minimum Free Heap、复位原因和最近错误。
- 页面显示 STA MAC、设备名称和 mDNS 主机名；验证 `http://lightgw.local`（或配置名）与当前 IPv4 均可访问。
- 从管理页触发重新配网，确认未认证请求被拒绝、旧 Wi-Fi 凭据被清除、随机 WPA2 热点启动且管理认证仍然有效。
- 将设备名改为 `lightgw-office1` 后确认 DHCP hostname、mDNS 地址和页面显示一致。
- Wi-Fi 断开、重连及 DHCP 地址变化后重新验证 mDNS 与 IPv4 访问。
- 连续断开/恢复 AP 三次，记录断线 reason 和重连时间。
- 断电 30 秒后配置仍存在，启动不改变任何灯具状态。
- 不接 DS3231 启动，确认 RTC 显示离线但 Wi-Fi/Web/手动控制正常。
- 接入经电平确认的 DS3231，验证 OSF、UTC 存储、北京时间显示、NTP 回写和管理员手动设置。
- 断开 Wi-Fi 后跨越计划时间：若此前时间可信，任务应继续；若从未获得可信 RTC/NTP/手动时间，任务必须跳过。

## Stage 2

- 串口显示 Stage 2、Coordinator 建网或恢复、PAN ID、信道。
- Web 开放 Permit Join 180 秒，页面倒计时与串口状态一致。
- Wi-Fi 持续 ping 与 HTTP 刷新时，观察 Zigbee 建网/Permit Join 是否异常。
- 分别在 Wi-Fi 空闲、高吞吐、弱 RSSI 条件记录 Free Heap、Minimum Free Heap、断线和 Zigbee 错误。

## Stage 3

- QBKG25LM 退出旧网络并复位，再执行配对。
- 保存从 `Device announce` 开始的完整日志。
- 记录所有 `active_endpoint`、`input_cluster`、`output_cluster` 和 model。
- 与实体左/中/右动作逐一对应，不能根据 Zigbee2MQTT 的 `state_left` 名称推断底层端点。
- 页面只能显示发现记录，控制按钮必须保持锁定。

## Stage 4

- 仅把 Stage 3 已实测的三个 On/Off server endpoint 保存为左/中/右映射。
- 每路执行开/关各 20 次，核对物理继电器和状态反馈。
- 实体按键每路 20 次，页面状态应更新。
- 对左/中/右分别设置最多 8 条任务，再设置可选“全部灯光”任务；验证同一分钟不同目标互不阻塞，指定目标只发送对应端点 OFF。
- 对空调 1、空调 2、共享空调组分别配置任务，验证只使用已保存的明确 OFF 码；IR 失败不得阻塞灯光。
- 验证启用、修改、删除、星期掩码、NVS 重启恢复和旧 v2 `sched_list` 到全局灯光目标的迁移。
- 验证 0/1/5/10 分钟有限重试、30 分钟补执行、不跨日、完成通道不重试，以及下班后人工重新开灯不被同一任务无限关闭。
- 依次覆盖 `tests/simulate_time_scheduler.py` 中 A～L 场景，并把真实硬件证据另行记录。
- Wi-Fi 断线/恢复、Zigbee 设备掉电/恢复、ESP32 断电/恢复。
- Wi-Fi 高吞吐下执行不少于 200 次 Zigbee 操作。
- 连续运行至少 72 小时，记录重启、coredump、Minimum Free Heap 趋势、丢包和最大响应时间。

## 每个 Stage 的资源检查

1. 使用全新独立构建目录依次执行 `idf.py build`、`idf.py size`、`idf.py size-components`，把结果、应用大小和 SHA-256 追加到 `BUILD.md` / `MEMORY_RESOURCE_REPORT.md`。
2. 冷启动后记录 Free Heap、Minimum Free Heap、Largest Free Block 和全部任务 Stack High Water Mark；任务列表若显示截断，必须先提高监控容量或减少任务后再判断。
3. Stage 2/3 在 Permit Join 前、设备 announce、端点发现完成后各保存一次页面和串口数据。
4. Stage 5 在开始学习、学习缓冲分配、每次采集和释放后保存数据；验证释放后的堆恢复趋势，并检查 Largest Free Block 是否持续缩小。
5. 并发运行 Wi-Fi、Zigbee、Web、RMT、DS3231 和后续两个 I2C 传感器；分别进行 Web 高频刷新、Zigbee 配对和红外学习，记录超时、掉线、复位及 coredump。
6. Stage 6 集成 RainMaker 前保存 Stage 5 基线；集成后分别记录 TLS 握手、MQTT 首连/重连前后的事件和最低堆。没有 Stage 6 实际构建或硬件日志时保持 `NOT TESTED`。
7. 对最低 HWM 任务执行其最坏路径；低于 512 B 的警告不能直接通过增大栈掩盖，应先确认是否有局部大数组或递归。

资源判定必须使用同一固件 SHA-256 和可复现负载。编译器的静态 DIRAM 统计、QEMU 或模拟测试不得写为硬件内存 `PASS`。

## 命名固件变体

1. 对 CORE、CORE_SENSOR、CORE_CLOUD、FULL 分别使用独立目录执行 `build`、`size`、`size-components` 并记录 SHA-256。
2. 检查 CORE 的生成组件清单不含 `sensors`、`rainmaker`，启动日志不出现扩展初始化。
3. 不接 BH1750/SHT31 启动 CORE_SENSOR，Core 必须继续工作；传感器缺失只记诊断。
4. RainMaker 网络不可用或扩展返回失败时，CORE_CLOUD/FULL 的 Wi-Fi、Web、本地调度和手动控制不得停止。
5. FULL→CORE 普通刷写后逐项检查 Wi-Fi、Zigbee 网络、红外码、RTC 配置和 56 条任务均保留；扩展分区不得被 CORE 初始化或擦除。
6. 拆除两个 Sensor 后确认 DS3231 继续通过共享 I²C 工作；不得由扩展删除或重配 Core 总线。
7. 按 `ROLLBACK.md` 执行完整备份、SHA-256 校验、回退和恢复演练。所有目标板项目完成前保持 `NOT TESTED`。

## Phase A 统一控制专项

- 烧录 `tests/phase_a_app/build-quality-c6/esp32lightctl_phase_a_tests.bin`，保存从启动到 `PHASE_A_C_TEST SUMMARY PASS: 16 tests` 的完整串口日志；16 项全部 PASS 才能把 C6 目标板测试状态从 `NOT TESTED` 改为 PASS。QEMU PASS 只能记为 `SIMULATED PASS`。
- 配对两台 Zigbee 设备（或一台设备重新配对并观察短地址变化），验证相同 endpoint 编号的状态不会串到另一 IEEE 设备。
- 先获得 OFF，再等待超过缓存有效期或制造回读超时；新 OFF 命令不得立即使用旧 OFF 显示 `CONFIRMED`。
- 回读超时后恢复设备并产生新报告，只有新报告到达后才能 `CONFIRMED`。
- 连续提交超过队列深度的命令，必须出现有界 `FAILED/queue full`，不得阻塞 HTTP/Zigbee 任务或触发无限重试。
- 同一分钟配置灯光与空调任务：灯光等待新鲜 Zigbee 确认；IR 必须等待 RMT 完成才是 `SENT`，且空调 actual state 始终为 `UNKNOWN`。

## Flash/NVS 故障专项

1. 先备份可恢复的测试配置，不在生产网络直接制造损坏。
2. 用故障注入测试验证 `ESP_ERR_NVS_NOT_ENOUGH_SPACE`、一般写失败和提交中断后仍读取最后一次完整记录；QEMU 中对应 C 测试必须全部 PASS。
3. 真实板分别制造主 NVS 与 `runtime_nvs` 空间不足，确认页面显示分区 stats/错误且串口明确报告；不得出现自动 erase。
4. 在单项 `exNN` 提交期间切断电源，恢复后只允许有界 OFF 重试，不得无限触发或把旧 OFF 当作本次成功。
5. 装载旧 `exec_v1`、旧 `log_v1`、v2 `sched_list`，在迁移前、中、后重启。逐项迁移必须可续传，旧数据必须保留；配置迁移失败不得清空用户任务。

## 配置备份与恢复

软件自动检查（`check-all.bat`）：

1. `tests/test_backup_contract.py` 检查三个认证 API、启动前恢复顺序、敏感字段排除、明确 OFF 语义、限流和 Web 安全提示。
2. `tests/test_backup_restore.py` 执行正常导入、结构错误、版本不兼容、校验失败、超大文件、模拟中途断电、重复导入和新板恢复模型。
3. `tests/phase_a_app` 编译真实 C SHA-256/篡改检测测试；只有烧录测试固件并看到 PASS 串口日志后，才能把执行状态从 NOT TESTED 改为 PASS。

硬件验证步骤（当前全部 **NOT TESTED**）：

1. 在旧板创建每个目标各 8 条（共 56 条）任务并学习两组有效 LG OFF 码，导出 JSON。
2. 确认文件中不存在 Wi-Fi/管理员明文、RainMaker 凭据、Zigbee network key、配对信息和端点映射。
3. 在新板烧录 CORE，使用新的 Wi-Fi/管理员首次配置后导入；确认 56 条任务和两个 IR 码恢复，Zigbee 页面仍要求重新配对。
4. 对格式错误、`format_version=99`、被修改但未更新摘要、超过 48 KiB、无管理员认证和 10 秒内重复上传分别验证明确错误，且原配置完全不变。
5. 在写入主 NVS 前、写入主 NVS 后、写入 `ir_data` 后、清除日志前四个窗口分别断电；每次重启应显示 interrupted import rollback，且恢复旧任务和旧 IR 码。
6. 注入 NVS 空间不足/提交失败；确认立即回滚或保留启动恢复日志，绝不擦除整个分区。
7. 依次测试 CORE_SENSOR、CORE_CLOUD、FULL 互相导入；未编入的扩展设置必须跳过并报告，核心设置不受影响。
8. 测量导出、解析 48 KiB 文件、验证候选驻留和提交时的 Free Heap/Minimum Free Heap/Largest Free Block；未测量前不得声称内存余量充足。
6. 配置 56 条任务并使其同分钟到期，验证各目标独立、最大重试有界、日志固定为 RAM 最近 8 条，运行 7 天后 NVS 用量不因普通日志线性增长。
7. 注入设备表 CRC/长度错误，确认自动覆盖被阻止；只有管理员明确执行恢复出厂设置并输入 `ERASE_ALL_USER_DATA` 时才允许擦除。
8. 对内存分配失败、Wi-Fi/Zigbee/RainMaker 掉线、IR 失败、RTC OSF、时间大幅跳变、系统复位分别保存页面、串口、coredump 和固件 SHA-256。尚未实现的 RainMaker 项保持 `NOT AVAILABLE / NOT TESTED`。

## 安全

QBKG25LM 连接市电，安装、拆卸和测量必须断电并由合格电工执行。ESP32 与开关之间只能通过 Zigbee 通信，禁止任何市电端子与开发板电气连接。
