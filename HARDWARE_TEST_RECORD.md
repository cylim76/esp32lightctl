# 硬件测试记录

硬件：ESP32-C6-DevKitC-1-N8 + Aqara QBKG25LM + 红外 RX/TX 模块 + LG 空调。以下未执行项目全部为 `NOT TESTED`。构建成功只记录在 `BUILD.md`。

| ID | Stage | 测试项 | 状态 | 日期/固件 | 证据或备注 |
|---|---:|---|---|---|---|
| HW-001 | 1 | USB 启动与串口日志 | NOT TESTED | — | 等待硬件 |
| HW-002 | 1 | 首次配置热点与 Web 配网 | NOT TESTED | — | 等待硬件 |
| HW-003 | 1 | Wi-Fi STA、IP、NTP、RSSI | NOT TESTED | — | 等待硬件 |
| HW-004 | 1 | HTTP Basic 未授权访问拦截 | NOT TESTED | — | 等待硬件 |
| HW-005 | 1 | NVS 断电持久化 | NOT TESTED | — | 等待硬件 |
| HW-006 | 1 | Wi-Fi 断线自动重连 | NOT TESTED | — | 等待硬件 |
| HW-007 | 1 | DHCP IPv4、STA MAC 与 RSSI 显示 | NOT TESTED | — | 等待硬件 |
| HW-008 | 1 | 默认及自定义主机名 mDNS 解析 | NOT TESTED | — | 等待硬件 |
| HW-009 | 1 | 断线/重连/IP 变化后的 mDNS 更新 | NOT TESTED | — | 等待硬件 |
| HW-010 | 1 | 管理员认证重新配网与临时 WPA2 AP | NOT TESTED | — | 等待硬件 |
| HW-011 | 1 | DS3231 3.3V 电平、I2C GPIO6/7 与 OSF | NOT TESTED | — | 先检查成品模块上拉/充电电路 |
| HW-012 | 1 | 无网络管理员手动设置 ESP32 与 DS3231 | NOT TESTED | — | 页面使用北京时间，RTC 存 UTC |
| HW-013 | 1 | RTC 断电恢复、NTP 02:00 同步及离线长期计时 | NOT TESTED | — | 构建/模拟通过不等于硬件通过 |
| HW-101 | 2 | Coordinator 首次建网与恢复 | NOT TESTED | — | 等待硬件 |
| HW-102 | 2 | Permit Join 180 秒 | NOT TESTED | — | 等待硬件 |
| HW-103 | 2 | Wi-Fi/Zigbee 单芯片 RF 共存 | NOT TESTED | — | 等待硬件 |
| HW-201 | 3 | QBKG25LM 加入网络 | NOT TESTED | — | 等待硬件 |
| HW-202 | 3 | Active Endpoint 记录 | NOT TESTED | — | 不预设 1/2/3 |
| HW-203 | 3 | 输入/输出 Cluster 记录 | NOT TESTED | — | 不预设 state_left 等上层名 |
| HW-204 | 3 | 左/中/右实际端点映射 | NOT TESTED | — | 控制保持锁定 |
| HW-301 | 4 | 三路独立 ON/OFF | NOT TESTED | — | 映射确认后执行 |
| HW-302 | 4 | 实体按键状态反馈 | NOT TESTED | — | 等待硬件 |
| HW-303 | 4 | 全部关闭 | NOT TESTED | — | 等待硬件 |
| HW-304 | 4 | NTP/断网定时安全策略 | NOT TESTED | — | 等待硬件 |
| HW-305 | 4 | Wi-Fi/Zigbee 掉线恢复 | NOT TESTED | — | 等待硬件 |
| HW-306 | 4 | 72 小时稳定性与内存趋势 | NOT TESTED | — | 等待硬件 |
| HW-307 | 4 | 多条每日关灯计划独立触发且不重复 | NOT TESTED | — | 测试 17:00、19:30、20:30、21:30、23:59 |
| HW-308 | 4 | 左/中/右各最多8条及同分钟独立 OFF | NOT TESTED | — | 不得调用全关代替指定端点 |
| HW-309 | 4 | 有效时间下离线运行、有限补执行与加班保护 | NOT TESTED | — | 不跨日、不无限重试 |
| HW-401 | 5 | GPIO18 TX / GPIO19 RX 电气与空闲状态 | NOT TESTED | — | 先确认模块电压和引脚 |
| HW-402 | 5 | LG 明确 OFF 三次稳定学习 | NOT TESTED | — | 不得以 Toggle 代替 |
| HW-403 | 5 | 两组红外码 NVS 持久化及拆除 RX | NOT TESTED | — | 等待硬件 |
| HW-404 | 5 | 单台 LG 学习码重放实际关机 | NOT TESTED | — | “已发送”不等于实际关闭 |
| HW-405 | 5 | 一个发射模块覆盖两台空调 | NOT TESTED | — | 先一台再两台 |
| HW-406 | 5 | 空调独立/复用灯光时间定时 | NOT TESTED | — | NTP、断网、同分钟防重复 |
| HW-407 | 5 | Wi-Fi + Zigbee + RMT 共存 | NOT TESTED | — | 记录 Heap、掉线与超时 |
| HW-408 | 5 | 1W 模块供电压降及 ESP32 复位 | NOT TESTED | — | 不假设板载5V可提供350mA |
| HW-409 | 5 | 红外功能72小时稳定性 | NOT TESTED | — | 等待硬件 |
| HW-410 | 5 | 空调1/空调2/共享组各最多8条独立定时 | NOT TESTED | — | 只用已确认 OFF 码，状态不声称已关闭 |
| HW-411 | 4/5 | Phase A/存储/备份/传感器协议 C 测试固件 18 项在目标板执行 | NOT TESTED | — | C6 测试镜像已 BUILD PASS；QEMU 18 项 SOFTWARE/SIMULATED PASS，尚未烧录 C6 |
| HW-412 | 4 | 多 Zigbee 设备/端点状态隔离与短地址复用 | NOT TESTED | — | 旧 OFF 不得串设备或确认新命令 |
| HW-413 | 4 | 缓存过期、主动回读超时及恢复后新反馈 | NOT TESTED | — | 页面应为 UNKNOWN/FAILED，收到新反馈后再确认 |
| HW-414 | 5 | IR 入队、RMT 完成、失败和结果超时状态链 | NOT TESTED | — | SENT 仍不代表空调已关闭 |
| HW-415 | 1 | Wi-Fi/Web/RTC 的 Free/Minimum Heap、Largest Block 与任务 HWM | NOT TESTED | — | 冷启动后记录，附固件 SHA-256 |
| HW-416 | 2 | Wi-Fi/Zigbee 共存、建网及 Permit Join 内存峰值 | NOT TESTED | — | 对照关键内存事件和 RF 日志 |
| HW-417 | 3 | Zigbee announce、端点/Cluster 发现内存峰值 | NOT TESTED | — | 保存发现完成/失败前后数据 |
| HW-418 | 4 | 三路控制、状态回读和调度并发内存/任务栈 | NOT TESTED | — | 包含回读超时和多个同分钟任务 |
| HW-419 | 5 | RMT 学习/发射及三个 I2C 设备并发资源峰值 | NOT TESTED | — | 传感器软件已集成；同时运行 Wi-Fi、Zigbee、Web 的实机峰值待测 |
| HW-420 | Cloud | RainMaker TLS 握手、MQTT 首连及重连内存峰值 | NOT TESTED | — | SDK/固件已集成，仅静态 size 已测；必须采集实机事件峰值 |
| HW-421 | 1～5 | 各 Stage 72 小时/7 天 Heap、Largest Block、最低 HWM 趋势 | NOT TESTED | — | 各自独立记录，不用编译结果代替 |
| HW-422 | 6 | 双 OTA 升级、回滚及 NVS/Zigbee/IR 数据迁移 | NOT TESTED | — | 当前仅有候选分区规划，尚未启用 |
| HW-423 | 1～5 | 主 NVS/runtime NVS 空间不足且不自动擦除 | NOT TESTED | — | 页面 stats、串口错误、配置保留均需实测 |
| HW-424 | 4/5 | 单项执行状态写入时断电和重启恢复 | NOT TESTED | — | QEMU 故障注入 PASS；真实 Flash 断电测试待执行 |
| HW-425 | 4/5 | 56 条任务、失败重试和 7 天日志/NVS 增长 | NOT TESTED | — | 验证无持久调试日志、无无限重试、记录 NVS entries |
| HW-426 | 1～5 | 用户确认恢复出厂设置及部分擦除失败处理 | NOT TESTED | — | 仅使用测试设备；确认文本必须精确匹配 |
| HW-427 | 5 | CORE 构建无扩展初始化/任务/专用缓冲 | NOT TESTED | — | 软件构建图已隔离，目标板待验证 |
| HW-428 | 5 | CORE_SENSOR 缺失 BH1750/SHT31 时 Core 正常启动 | NOT TESTED | — | 缺失只能记录诊断 |
| HW-429 | 5/Cloud | CORE_CLOUD/FULL 云不可用时本地控制与调度正常 | NOT TESTED | — | 传输路径已实现；断云隔离和重连须实机验证 |
| HW-430 | 5 | FULL→CORE 保留 Wi-Fi/Zigbee/IR/RTC/56 条任务 | NOT TESTED | — | 先按 ROLLBACK.md 完整备份 |
| HW-431 | 5 | 移除 BH1750/SHT31 后 DS3231 共享 I²C 正常 | NOT TESTED | — | 记录总线和 OSF 日志 |
| HW-432 | 5 | 8 MiB 备份、变体回退及恢复演练 | NOT TESTED | — | 禁止 erase-flash，校验所有 SHA-256 |
| HW-433 | 1～5 | 管理员认证后的配置导出、上传限流、48 KiB 限制与两阶段确认 | NOT TESTED | — | 浏览器和目标板 Web 实测待执行；仅在可信局域网使用 |
| HW-434 | 1～5 | 配置导入写入中断电后的事务回滚 | NOT TESTED | — | 主配置、56 条任务和两组 IR 码都必须恢复到导入前状态 |
| HW-435 | 3～5 | 新开发板导入备份后重新配对、重新发现端点并验证两组 LG OFF 码 | NOT TESTED | — | 禁止把旧 Zigbee 映射或配对信息当作有效网络数据恢复 |
| HW-436 | Sensor | BH1750 实际 Lux、五场景标定与整体夜间异常 | NOT TESTED | — | 单传感器不得断言具体灯路 |
| HW-437 | Sensor | SHT31 温湿度、CRC 错误、趋势与拔插恢复 | NOT TESTED | — | 不得把温度趋势虚报为空调状态 |
| HW-438 | Sensor | BH1750/SHT31 独立启停、重启恢复与旧数据失效 | NOT TESTED | — | 记录 Heap、任务 HWM 和 NVS 写入计数 |
| HW-439 | Sensor | DS3231+BH1750+SHT31 GPIO6/7 共享总线 | NOT TESTED | — | 检查上拉电压、地址、OSF 与并发恢复 |
| HW-440 | Variants | CORE 页面显示扩展未包含且按钮禁用 | NOT TESTED | — | Core 控制必须继续正常 |

状态变更时必须附串口日志文件名、测试条件和可复现步骤；FAIL 不得删除，应追加修复版本和复测结果。
