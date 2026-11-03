# 系统架构

## 长期 Core 与扩展边界

Core 固定包含 Wi-Fi、Web、Zigbee/Aqara、LG 红外、DS3231 和本地调度。BH1750、SHT31 与 RainMaker 是三个可删除扩展，分别位于 `extensions/bh1750`、`extensions/sht31` 和 `extensions/rainmaker`；Core 不反向依赖它们。共享 I²C 由 `components/lightgw_i2c_bus` 所有，DS3231 不依赖传感器组件。四种固件和安全边界见 [FIRMWARE_VARIANTS.md](FIRMWARE_VARIANTS.md)，回退见 [ROLLBACK.md](ROLLBACK.md)。

## 单源码阶段边界

`LIGHTGW_STAGE=1..5` 使用同一套源码和独立构建目录。Stage 1 仅含 Wi-Fi、Web、NVS、RTC/时间；Stage 2 加 Coordinator；Stage 3 加发现诊断；Stage 4 加统一设备控制与调度；Stage 5 再加红外。RTC 硬件是可选项，缺失不会阻止其他模块启动。既有 `firmware/` 发布镜像不由回归构建覆盖。

## Phase A 控制数据流

```text
Web / scheduler / optional RainMaker adapter
              |
              v
       device_control queue
        |              |
        v              v
 Zigbee endpoint     IR task queue
 command + read      RMT completion
        |              |
        v              v
 per IEEE/endpoint   SENT/FAILED
 fresh state cache   (physical AC UNKNOWN)
        |
        v
 CONFIRMED/FAILED command result
```

Web 和调度器不再直接调用 Zigbee 或 RMT。`device_control.c` 是唯一上层命令入口；`zigbee_manager.c` 和 `ir_manager.c` 仅提供底层执行与反馈。后续 RainMaker 必须复用此入口，不能复制一套控制器或定时器。

## 状态与并发

- `device_state.c` 保存最近 32 个命令的 `PENDING / SENT / CONFIRMED / FAILED / UNKNOWN` 生命周期。
- Zigbee 缓存以 IEEE 地址、短地址和端点共同隔离。每条记录有更新时间、配对代次、回读等待和回读超时状态。
- 发送 Zigbee 命令前先使该端点旧缓存失效；只有命令发送时间之后收到的匹配端点反馈才可确认。
- 设备 announce、短地址复用、重新发现、缓存超过 6 分钟或回读超过 5 秒都会使旧值不可用于确认。
- 红外入队仍是 `PENDING`；RMT 完成才是终态 `SENT`。由于没有空调反馈，实际状态固定为 `UNKNOWN`。
- `settings_copy()` 为调度器/Web 提供互斥快照，避免保存计划时读取半更新结构。NVS 写入只发生在配置、迁移和有限的每日执行状态变化时，不按轮询周期写入。

## 调度可靠性

七个目标各最多 8 条、总计 56 条计划。每个计划提交独立命令 ID；一个目标失败不阻塞其他目标。重试次数、间隔和补执行窗口都有上限，完成后当日不再自动关闭，避免加班人员重新开灯后被无限关闭。执行状态写入 NVS，用于重启和时间回拨去重；不跨日补发。

旧版独立 IR 定时在启动时迁移到统一计划并停用旧执行器，因此不会与 `scheduler.c` 重复发射。迁移失败时保留旧数据并明确报错，但旧执行器不会继续运行造成重复关机。

## 验证边界

Phase A 已通过 Stage 1～5 编译、网页 JavaScript 解析、A～L 时间/调度模拟，以及真实生产 C 模块的 8 项 ESP32-C3 QEMU 执行。QEMU 结果属于 `SIMULATED PASS`；同一测试工程另生成 ESP32-C6 可烧录镜像，但目标板尚未执行。Zigbee/RMT/DS3231 的时序、电气和 RF 行为仍以 `HARDWARE_TEST_RECORD.md` 为准。
