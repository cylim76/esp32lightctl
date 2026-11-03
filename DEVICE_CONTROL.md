# 统一设备控制服务

## 接口

上层只调用：

- `device_control_submit_light(short_addr, channel, on, source, &id)`：指定 D1 和指定通道；
- `device_control_submit_target(target, source, &id)`：计划目标或全部灯光明确 OFF；
- `device_control_submit_target_repeated(target, repeats, source, &id)`：有限重复的空调 OFF；
- `device_control_get_result(id, &result)`：非阻塞查询。

HTTP 提交返回命令 ID 和 `PENDING`。`GET /api/control/result?id=N` 返回终态、设备实际状态和原因。命令历史是容量 32 的环形缓存；过旧 ID 返回 404，而不会错误关联到新命令。

## 状态语义

| 状态 | 含义 |
|---|---|
| `PENDING` | 已排队，尚未得到底层完成结果 |
| `SENT` | 底层发送已完成；对红外是终态，但空调实际状态仍为 `UNKNOWN` |
| `CONFIRMED` | Zigbee 目标端点均给出本次命令之后的新鲜匹配反馈 |
| `FAILED` | 无目标、队列满、底层失败、回读超时或确认超时 |
| `UNKNOWN` | 没有可确认的设备状态；不能等同 OFF |

## Zigbee 状态隔离与有效期

`zigbee_state_cache.c` 的每条记录分别保存 IEEE、短地址、endpoint、值、更新时间、回读期限和配对代次。默认状态有效期 360 秒、主动回读超时 5 秒、命令确认总超时 7 秒。

以下情况一律拒绝旧 OFF 作为本次成功证据：

1. 命令发送前缓存已有 OFF；开始主动回读时立即将其置为无效。
2. 缓存超过有效期。
3. 主动回读到达期限仍无响应。
4. 同一 IEEE 重新 announce/配对并获得新短地址。
5. 短地址被另一个 IEEE 设备复用。
6. 反馈属于另一个设备或另一个 endpoint。

实体按键的标准 On/Off 属性报告进入同一缓存并更新对应设备、对应端点。Coordinator 恢复网络时对已保存映射重新读取；设备 announce 后重新发现并清除旧状态。

## 红外结果

每个 IR 作业具有独立 task ID、码槽、重复次数、来源、排队时间、完成时间和结果。队列成功不代表发送成功；`ir_manager` 只有在 RMT 重放完成后记录 `IR_TASK_SENT`。调度器收到 `SENT` 后可结束本次计划，但页面明确显示“已发送，空调实际状态未知”。

## 测试

`tests/phase_a_app` 直接编译生产代码 `zigbee_state_cache.c` 和 `device_state.c`，覆盖：多设备同端点隔离、过期 OFF、回读超时、超时后新反馈恢复、重新配对代次、短地址复用、命令生命周期和环形历史拒绝覆盖未完成命令。8 项已在 ESP-IDF 官方 ESP32-C3 QEMU 中执行通过，记为 `SIMULATED PASS`。烧录 C6 测试固件后串口仍必须出现全部 `PHASE_A_C_TEST PASS` 与最终汇总；目标板执行前保持 `NOT TESTED`。
