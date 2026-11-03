# Stage 5 技术方案

## 兼容边界

- Stage 1～4 的功能宏和原始 `partitions.csv` 保留。
- 只有 `LIGHTGW_STAGE=5` 编译 RMT 与红外模块，并叠加 `sdkconfig.stage5`。
- 已发布的 `firmware/stage1`～`stage4` 不改写；Stage 5 使用独立 `build-stage5` 和 `firmware/stage5`。
- Stage 1～4 版本保持 `0.4.0-stageN`，Stage 5 为 `0.5.0-stage5`。

## GPIO 与 RMT

- TX：GPIO18，RMT TX，默认 active-high。
- RX：GPIO19，RMT RX，默认解调 mark 为低电平。
- 分辨率：1MHz，即存储单位为 1µs。
- RX 缓冲：最多 512 个 RMT symbol / 1024 个 mark-space duration；小于 80µs 的脉冲按噪声过滤，25ms 连续电平结束当前捕获。
- TX：copy encoder 重放规范化后的完整 duration 序列；载波保存为每个码的元数据，允许 30～60kHz，默认 38kHz/33%。发射结束使用 RMT EOT 空闲电平。

ESP32-C6 每个 RMT channel 可独立配置为 RX 或 TX。RX 学习结束立即 disable/delete channel 并复位 GPIO，因此保存完成后可拆除接收模块。TX channel 常驻但只有队列任务能够调用。

## 学习状态机

```text
Web start
  -> 创建临时 RX channel
  -> 捕获明确 OFF
  -> 规范化为 mark/space µs 数组
  -> 长度一致且每段在 max(200µs, 25%) 容差内？
       否：以最新样本重新计数
       是：累计并平均
  -> 连续稳定 3 次
  -> 候选待用户确认
  -> 设置发射载波/占空比并保存 NVS
  -> 释放 RX
```

最多尝试8次，每次等待20秒。没有达到三次一致不会覆盖已保存的正式码。

## 协议与原始码

固件首先保存原始 mark/space，因此协议解码失败不影响重放。识别器只对常见的 8ms/4ms、28-bit LG-like 波形做候选解析，并检查末尾校验 nibble；结果分为：

- `LG_28BIT_CHECKSUM_OK`
- `LG_LIKE_28BIT_UNCONFIRMED`
- `RAW_UNDECODED`

这些名称不是空调型号兼容认证。普通 38kHz 解调接收头已经去除载波，只能记录基带包络，`carrier_measured` 永远为 false；38kHz 是配置/估计值。

## 存储

Stage 5 在原 coredump 之后新增 64KB `ir_data` NVS 分区。每个槽位保存固定版本结构、最多1024段 duration、采样分辨率、载波、占空比、接收极性、协议候选、长度、总时长和 CRC32。两个槽位合计约4KB，给 NVS 日志和后续版本留有充足空间。

原分区地址：

| 分区 | 地址 | 大小 |
|---|---:|---:|
| factory | 0x10000 | 0x300000 |
| zb_storage | 0x310000 | 0x40000 |
| zb_fct | 0x350000 | 0x1000 |
| coredump | 0x351000 | 0x10000 |
| ir_data（Stage 5） | 0x361000 | 0x10000 |

## 发送与调度

HTTP 只做参数校验并向深度4的 FreeRTOS 队列提交任务。独立 `ir_tx_manager` 任务加载并校验 CRC，然后执行 RMT；不会在 HTTP 或 Zigbee 回调中等待发射。

- 单帧总时长上限300ms。
- 重复次数限制为1～2次，重复间隔固定100ms且不低于50ms。
- 两个不同码之间间隔250ms。
- 学习期间拒绝发射；有待发射任务时拒绝开始学习。
- 结果只报告 queued、RMT sent 或 failed，永不把 RMT sent 写成 AC closed。

空调调度器与灯光调度器是不同任务。它可以使用一个独立的星期/时间，也可复用所有已启用灯光计划；同一分钟只入队一次。Wi-Fi 断开或本次启动未可靠 NTP 校时时跳过且不补发。红外错误不会阻塞灯光 `all_off`。

## 未经硬件验证的风险

- 接收模块的真实供电电压、输出电平和 mark 极性。
- 1W 模块是否内置正确的限流/功率驱动，以及标称350mA的测试条件。
- 38kHz/33% 是否适合具体模块和两台 LG 空调。
- 两台空调是否使用同一 OFF 帧、是否需要不同重复间隔。
- 1W 发射瞬间的5V压降是否导致 ESP32 brownout。
- Wi-Fi、Zigbee 和 RMT 同时活动时的现场延迟与丢包。

上述项目全部保持 `NOT TESTED`，按 [IR_TEST_PLAN.md](IR_TEST_PLAN.md) 上板验证。
