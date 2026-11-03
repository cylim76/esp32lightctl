# Stage 5 红外硬件测试计划与日志格式

软件构建通过不能记为硬件 PASS。每项只允许 `PASS`、`FAIL`、`NOT TESTED`，失败记录不得删除，应追加修复提交和复测结果。

## 推荐顺序

1. 不连接任何红外模块，烧录 Stage 5，确认 Wi-Fi、Web、Zigbee、三路灯光和 Heap 无回归。
2. 核对发射/接收模块型号、引脚、电压、输出电平和发射模块峰值电流。
3. 只连接接收模块，连续学习同一个明确 OFF 键三次。
4. 重启并确认已保存码仍存在，然后拆除 RX，确认网关正常启动。
5. 先用示波器/逻辑分析仪或安全的低功率红外 LED 检查 GPIO18，不直接从第一次测试就驱动 1W 模块。
6. 接入有独立余量的 5V 发射电源，先验证一台空调，再验证一个模块覆盖两台。
7. 最后测试定时、Wi-Fi/Zigbee 并发、电源压降和长时间稳定性。

## 测试矩阵

| ID | 测试项 | 初始状态 | 通过条件 |
|---|---|---|---|
| IR-001 | Stage 5 无模块启动 | NOT TESTED | 无重启；Stage 1～4 功能正常 |
| IR-002 | GPIO18 空闲电平 | NOT TESTED | 启动、发送结束、异常后均不持续驱动 LED |
| IR-003 | RX 电压与逻辑电平 | NOT TESTED | GPIO19 输入不超过 3.3V，mark/space 极性与配置一致 |
| IR-004 | 三次 OFF 学习稳定性 | NOT TESTED | 三次长度和每段时序在容差内，候选才可保存 |
| IR-005 | 长帧/缓冲边界 | NOT TESTED | 无截断、溢出或 `ESP_ERR_INVALID_SIZE` |
| IR-006 | LG 协议候选识别 | NOT TESTED | 日志准确区分 LG 候选、校验通过或 RAW，不能误报实测载波 |
| IR-007 | 两组码 NVS 持久化 | NOT TESTED | 断电重启后长度、协议、载波和 CRC 保持一致 |
| IR-008 | 拆除 RX 后运行 | NOT TESTED | 启动、手动发送和定时不依赖接收模块 |
| IR-009 | GPIO18 RMT 波形 | NOT TESTED | mark 有配置载波，space 无载波，结束后回空闲 |
| IR-010 | 单台 LG 明确 OFF | NOT TESTED | 空调实际由开机变为关机；不能用 Toggle 代替 |
| IR-011 | 两台相同型号同时关闭 | NOT TESTED | 一个已验证码和一次发射覆盖两台 |
| IR-012 | 两台不同型号顺序关闭 | NOT TESTED | 码1、码2顺序发送且间隔受限，两台都实际关闭 |
| IR-013 | HTTP 异步队列 | NOT TESTED | 请求快速返回；页面区分“已提交/已发送/实际状态未知” |
| IR-014 | 连续发射限制 | NOT TESTED | 最多2次重复、间隔不少于50ms，无持续导通或过热 |
| IR-015 | 独立空调定时 | NOT TESTED | 星期和时间正确，同一分钟只触发一次 |
| IR-016 | 复用灯光时间 | NOT TESTED | 灯光和红外分别执行，任一失败不阻塞另一任务 |
| IR-017 | NTP/断网保护 | NOT TESTED | 未可靠校时或断网时跳过且不补发，页面显示原因 |
| IR-018 | Wi-Fi+Zigbee+RMT 共存 | NOT TESTED | 发射/学习期间无异常掉线、Zigbee 超时或 coredump |
| IR-019 | 5V 发射电源压降 | NOT TESTED | 发射瞬间 ESP32 不复位，5V/3.3V 在模块规格范围 |
| IR-020 | 72小时稳定性 | NOT TESTED | 无异常重启；Minimum Free Heap 无持续下降 |

## 学习日志格式

保存从点击“开始学习”到候选保存完成的完整串口输出：

```text
firmware_commit=
board=
rx_module_model=
rx_supply_voltage=
rx_output_high_voltage=
remote_model=
remote_key=explicit OFF
slot=
capture_1_duration_count=
capture_1_total_us=
capture_2_duration_count=
capture_2_total_us=
capture_3_duration_count=
capture_3_total_us=
protocol_result=
carrier_hz_configured_or_estimated=
carrier_measured=false
saved_crc32=
result=PASS|FAIL|NOT TESTED
log_file=
notes=
```

## 发射日志格式

```text
firmware_commit=
tx_module_model=
tx_supply_source=
tx_supply_idle_voltage=
tx_supply_min_voltage_during_send=
slot_mask=
carrier_hz=
duty_percent=
repeat_count=
queue_submit_result=
rmt_completion_result=
ac1_actual_result=OFF|NOT OFF|UNKNOWN
ac2_actual_result=OFF|NOT OFF|UNKNOWN
wifi_disconnects=
zigbee_timeouts=
minimum_free_heap_before=
minimum_free_heap_after=
result=PASS|FAIL|NOT TESTED
log_file=
notes=
```

“RMT completion result=sent”只证明波形提交与外设发送完成，不能替代 `ac*_actual_result` 的现场观察。
