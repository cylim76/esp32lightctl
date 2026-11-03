# 硬件测试记录

硬件：ESP32-C6-DevKitC-1-N8 + Aqara QBKG25LM。当前开发板尚未到货，以下项目全部为 `NOT TESTED`。构建成功只记录在 `BUILD.md`。

| ID | Stage | 测试项 | 状态 | 日期/固件 | 证据或备注 |
|---|---:|---|---|---|---|
| HW-001 | 1 | USB 启动与串口日志 | NOT TESTED | — | 等待硬件 |
| HW-002 | 1 | 首次配置热点与 Web 配网 | NOT TESTED | — | 等待硬件 |
| HW-003 | 1 | Wi-Fi STA、IP、NTP、RSSI | NOT TESTED | — | 等待硬件 |
| HW-004 | 1 | HTTP Basic 未授权访问拦截 | NOT TESTED | — | 等待硬件 |
| HW-005 | 1 | NVS 断电持久化 | NOT TESTED | — | 等待硬件 |
| HW-006 | 1 | Wi-Fi 断线自动重连 | NOT TESTED | — | 等待硬件 |
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

状态变更时必须附串口日志文件名、测试条件和可复现步骤；FAIL 不得删除，应追加修复版本和复测结果。
