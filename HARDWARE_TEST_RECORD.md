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
| HW-411 | 4/5 | Phase A C 测试固件 8 项在目标板执行 | NOT TESTED | — | 测试镜像已 BUILD PASS，尚未烧录运行 |
| HW-412 | 4 | 多 Zigbee 设备/端点状态隔离与短地址复用 | NOT TESTED | — | 旧 OFF 不得串设备或确认新命令 |
| HW-413 | 4 | 缓存过期、主动回读超时及恢复后新反馈 | NOT TESTED | — | 页面应为 UNKNOWN/FAILED，收到新反馈后再确认 |
| HW-414 | 5 | IR 入队、RMT 完成、失败和结果超时状态链 | NOT TESTED | — | SENT 仍不代表空调已关闭 |

状态变更时必须附串口日志文件名、测试条件和可复现步骤；FAIL 不得删除，应追加修复版本和复测结果。
