# ESP32-C6 内存与 Flash 资源报告

## 结论边界

本报告记录的是 `ESP-IDF 5.5.1` 对 `0.10.0-stageN` 和四种命名变体的最新真实构建结果，并保留早期版本的对照数据。`idf.py size` 的静态占用不能证明设备在 Wi-Fi、Zigbee、RMT、I2C 或未来 RainMaker 并发运行时具有足够余量。所有运行时峰值和长期稳定性项目在开发板实测前均为 `NOT TESTED`。

ESP32-C6-DevKitC-1-N8 标称有 512 KB HP SRAM，但链接器本次报告可参与 DIRAM 统计的区域为 452,112 B。下表不能用 512 KB 直接减去静态占用来推断运行时 Free Heap；Wi-Fi/Zigbee 驱动、任务栈、TLS、网络缓冲和碎片化都需要上板测量。

## 复现命令

五个 Stage 使用同一源码和互相独立的构建目录。2026-11-03 已对每个 Stage 真实执行以下三类命令，退出码均为 0：

```bat
idf.py -B build-resource-stageN -D LIGHTGW_STAGE=N build
idf.py -B build-resource-stageN size
idf.py -B build-resource-stageN size-components
```

也可在已经构建的标准目录中执行：

```bat
memory-report.bat 1
memory-report.bat 2
memory-report.bat 3
memory-report.bat 4
memory-report.bat 5
```

## Stage 1～5 静态资源结果

`Flash` 是 `size` 报告的非 RAM Flash 总量（代码、只读数据和 app descriptor）；`DIRAM` 是 IRAM 代码、`.data` 和 `.bss` 的合计。应用 BIN 大小包含镜像格式/对齐，因此与 `size` 的总镜像字节数略有差异。

| Stage | 构建 | 应用 BIN | Flash | DIRAM 静态 / 452,112 B | DIRAM 组成（text/data/bss） | BIN SHA-256 |
|---:|---|---:|---:|---:|---:|---|
| 1 | PASS (BUILD ONLY) | 1,069,504 B (`0x1051c0`) | 942,208 B | 159,960 B (35.38%) | 112,460 / 14,740 / 32,760 B | `a9b04d071750b7113f848fdd79316d2dd2763e98f2f67634bb545bae05b645df` |
| 2 | PASS (BUILD ONLY) | 1,430,464 B (`0x15d3c0`) | 1,294,916 B | 191,198 B (42.29%) | 120,114 / 15,332 / 55,752 B | `a27bd35e35ba16005e8a5baeb7a423a159c0b9bf387b27826607f0281b62d5f8` |
| 3 | PASS (BUILD ONLY) | 1,433,808 B (`0x15e0d0`) | 1,298,270 B | 191,198 B (42.29%) | 120,114 / 15,332 / 55,752 B | `f5a1e2ae9363634ce62bd86c5739419a227fb2bd0edf58855ef79459fd713f0c` |
| 4 | PASS (BUILD ONLY) | 1,447,424 B (`0x161600`) | 1,311,876 B | 203,670 B (45.05%) | 120,114 / 15,332 / 68,224 B | `86963ac01d47f347ac391408bfd17e35dd6666403fe33e8bfa71e30cbae90b66` |
| 5 | PASS (BUILD ONLY) | 1,483,408 B (`0x16a290`) | 1,343,568 B | 212,822 B (47.07%) | 123,742 / 16,000 / 73,080 B | `d71d044c5f0973864473249e5336ec9788996835986bd8378b860612cc599ed5` |

Stage 5 相对 Stage 1 增加 401,360 B Flash 和 52,862 B 静态 DIRAM。相邻 Stage 的主要变化是 Stage 2 引入 Zigbee/ZBOSS，Stage 4 引入控制、调度和状态模块，Stage 5 引入 RMT/红外模块。该差值仍不是运行时峰值。

`size-components` 显示 Stage 5 的主要 Flash 贡献包括 ZBOSS 307,437 B、Wi-Fi `net80211` 177,947 B、lwIP 121,915 B、`esp_timer` 110,060 B、Wi-Fi `pp` 109,129 B、mbedTLS crypto 79,816 B、WPA supplicant 78,666 B 和本项目 `main` 70,752 B。主要静态 RAM 贡献包括 Wi-Fi `pp` 47,022 B、本项目 `main` 30,966 B、FreeRTOS 19,310 B、`net80211` 19,069 B、硬件支持 12,645 B、ZBOSS 12,393 B、HAL 12,436 B、SPI Flash 10,838 B 和 IEEE 802.15.4 10,517 B。组件值用于定位趋势，不代表组件的动态堆峰值。

## 运行时监控实现

- Web 主状态和 `/api/diagnostics/resources` 显示 `Free Heap`、启动以来 `Minimum Free Heap`、`Largest Free Block`。
- FreeRTOS trace facility 在所有 Stage 启用。资源接口按任务返回状态、优先级和 `Stack High Water Mark`（words 与 bytes）；每 60 秒串口输出最低剩余栈。低于 512 B 时输出警告。
- 任务快照上限为 48 个，内存关键事件使用固定 16 项环形缓冲；超过任务容量会明确标记截断，不会为历史数据无限分配 RAM。
- 已记录启动、Wi-Fi 启动/获得 IP/断线、Zigbee 启动/建网/Permit Join/设备 announce/发现完成或失败、红外学习缓冲分配/每次采集/结束和红外发送前后。
- `/api/status` 保持轻量；完整任务列表由独立鉴权接口按 15 秒刷新，避免每 3 秒构造大型 JSON。
- Web 页面由 CMake `EMBED_FILES` 嵌入只读 Flash，不在 RAM 长期保存第二份网页。运行时 JSON 仍有短时分配，必须通过 Largest Free Block 与事件快照实测其碎片化影响。

硬件记录时至少保存以下数据：动作前、动作中最小值、动作完成后恢复值、最低任务 HWM、是否复位/断线、固件 SHA-256。`Minimum Free Heap` 只会下降，因此每个测试场景应从冷启动开始，不能拿不同启动周期的最小值直接比较。

## Stage 6 / RainMaker 前后基线

| 对比点 | 固件 | Flash | 静态 DIRAM | TLS/MQTT 运行峰值 | 状态 |
|---|---|---:|---:|---:|---|
| 集成 RainMaker 前 | Stage 5 `0.8.0-stage5` | 1,343,568 B | 212,822 B | 未测 | BUILD PASS / HARDWARE NOT TESTED |
| 集成 RainMaker 后 | 尚未实现的 Stage 6 | 待真实 `size` | 待真实 `size` | 待开发板握手实测 | NOT AVAILABLE / NOT TESTED |

Stage 6 合入条件：在独立 `build-stage6` 中重新执行 `build`、`size`、`size-components`；在 TLS/MQTT 初始化前、TLS 握手开始/完成/失败、MQTT 重连前后记录事件；对比首次配网、稳定连接和断网退避三个场景。当前工程不含 RainMaker，因此不能给出伪造的“集成后”数值。

## 并发资源硬件测试矩阵

| 场景 | 需要同时运行 | 关键观测 | 当前状态 |
|---|---|---|---|
| 基线 | Wi-Fi + Web + RTC/I2C | free/min/largest、所有任务 HWM | NOT TESTED |
| Zigbee 配对峰值 | Wi-Fi + Web + Zigbee steering/discovery | announce 到 discovery done 事件、丢包、最低堆 | NOT TESTED |
| 红外学习峰值 | Wi-Fi + Zigbee + RMT RX + 学习缓冲 | buffers ready、三次 capture、释放后恢复 | NOT TESTED |
| 全外设并发 | Wi-Fi + Zigbee + Web + RMT + DS3231 + 后续两个传感器 | 堆碎片、任务栈、I2C/RF 超时 | NOT TESTED |
| RainMaker | 上述负载 + TLS/MQTT | 首次握手峰值、重连峰值、MQTT 缓冲 | NOT TESTED / Stage 6 未实现 |
| 长稳 | 全功能 72 小时及 7 天 | min heap 趋势、largest block、重启和 coredump | NOT TESTED |

后续环境传感器只允许固定长度短期趋势环形缓冲，不保存完整采样历史；批量上传使用有上限的分块缓冲。大型历史数据应留在外部平台或按需持久化，不能在 SRAM 中无限增长。

## 8 MB 双 OTA 分区评估

现行 Stage 1～4/5 分区是 factory-only，为保持已发布固件和 Zigbee/NVS 地址兼容，本次不直接替换。下列只是 Stage 6 前的候选规划，尚未进行 OTA、回滚或旧数据迁移硬件测试：

| 分区 | 偏移 | 大小 | 用途 |
|---|---:|---:|---|
| nvs | `0x009000` | `0x010000` (64 KB) | Wi-Fi、设置、设备数据 |
| otadata | `0x019000` | `0x002000` (8 KB) | OTA 选择数据 |
| phy_init | `0x01B000` | `0x001000` (4 KB) | PHY |
| padding | `0x01C000` | `0x004000` | 应用 64 KB 对齐 |
| ota_0 | `0x020000` | `0x300000` (3 MB) | 应用槽 A |
| ota_1 | `0x320000` | `0x300000` (3 MB) | 应用槽 B |
| zb_storage | `0x620000` | `0x040000` (256 KB) | Zigbee 网络数据 |
| zb_fct | `0x660000` | `0x001000` (4 KB) | Zigbee factory data |
| coredump | `0x661000` | `0x010000` (64 KB) | ELF coredump |
| ir_data | `0x671000` | `0x010000` (64 KB) | 两组红外码 |
| 预留 | `0x681000`～`0x7FFFFF` | `0x17F000` | RainMaker/传感器/迁移余量，待设计 |

Stage 5 的 0.8.0 基线 BIN 为 1,483,408 B；0.9.0 复测为 1,493,872 B，均能放入候选 3 MB OTA 槽。这不能证明加入 RainMaker 后仍满足空间、签名、回滚和迁移要求。启用该表前必须确认最终 Stage 6 镜像、bootloader OTA 配置、加密/签名开销，并设计从现行 factory-only 地址迁移 NVS、Zigbee 与 IR 数据的可恢复流程。

## 0.9.0 存储安全版本静态资源复测

2026-11-03 对最终存储安全源码重新执行 `idf.py size` 和 `size-components`，两类命令在 Stage 1～5 均退出 0：

| Stage | Flash Code | 静态 DIRAM | `.data` | `.bss` | DIRAM 剩余（链接估算） |
|---|---:|---:|---:|---:|---:|
| 1 | 949,114 B | 160,356 B | 14,736 B | 33,160 B | 291,756 B |
| 2 | 1,302,316 B | 192,386 B | 15,320 B | 56,952 B | 259,726 B |
| 3 | 1,305,682 B | 192,386 B | 15,320 B | 56,952 B | 259,726 B |
| 4 | 1,321,844 B | 206,738 B | 15,320 B | 71,304 B | 245,374 B |
| 5 | 1,354,022 B | 215,902 B | 16,000 B | 76,160 B | 236,210 B |

`runtime_nvs` 是 Flash 分区，不占用上述静态 DIRAM；调度持久化缓存和 RAM 日志已经计入 Stage 4/5 的 `.bss`。这些数字仍不能证明 Wi-Fi、Zigbee、RMT、RTC 及未来 RainMaker 同时运行时的动态堆余量；实机峰值项目保持 `NOT TESTED`。

## 0.10.0 模块化变体复测

2026-11-03 使用独立 `build-quality-variant-*` 目录，对四种变体实际执行 `build`、`size` 和 `size-components`，全部退出 0。这里的 Cloud 只是 RainMaker 单向适配边界，不包含真实 TLS/MQTT，因此不能作为 RainMaker 集成后内存数据。

| 变体 | 应用 BIN | Flash | 静态 DIRAM | data / bss | BIN SHA-256 |
|---|---:|---:|---:|---:|---|
| CORE | 1,494,272 B | 1,354,422 B | 215,918 B | 16,008 / 76,168 B | `2a84731adc2fd6999152415e1bd22112fa1292c0c0a0785302b591c980635e57` |
| CORE_SENSOR | 1,495,168 B | 1,355,312 B | 215,918 B | 16,008 / 76,168 B | `f5bac787f51dfd64aa91d6ae4ddd867720f1c72b503902d0859ee35cb2147992` |
| CORE_CLOUD | 1,494,800 B | 1,354,956 B | 215,918 B | 16,008 / 76,168 B | `019b828c607fb6d6b4bf8513ba53ef01a4028dd6c025253d17b8c35ae855c1c3` |
| FULL | 1,495,552 B | 1,355,696 B | 215,918 B | 16,008 / 76,168 B | `36e9f1c50570dafe23e23ae40d28262ea5addb7fc8ae3239ce257c7e616b2e35` |

相对 CORE，当前两个独立探测型传感器组件合计增加 890 B Flash/0 B 静态 DIRAM，Cloud 占位适配层增加 534 B Flash/0 B 静态 DIRAM。该差值只反映当前占位实现；真正的传感器采样缓存与 RainMaker TLS/MQTT 尚未实现，未来必须重新测量。CORE 的生成组件清单不含 `bh1750`、`sht31` 或 `rainmaker`。

## 0.11.0 备份恢复版本复测

2026-11-04 的 `check-all.bat` 对九个独立构建目录实际执行 build/size/size-components，全部退出 0：

| 构建 | BIN | Flash | 静态 DIRAM | data / bss | SHA-256 |
|---|---:|---:|---:|---:|---|
| Stage 1 | 1,092,992 | 965,688 | 160,400 | 14,748 / 33,192 | `974c8cddb877139c99a6ef899b804db5efbbaf0084de6c34c0c9bd29684753dc` |
| Stage 2 | 1,454,480 | 1,318,934 | 192,438 | 15,340 / 56,984 | `6ed9f041591dfe75a9af23ae87adce4f7223ecd47158bba6209fb369d1d701d5` |
| Stage 3 | 1,457,952 | 1,322,400 | 192,438 | 15,340 / 56,984 | `772ca2e35593c89d29c04c0b720d6fc339eac28dc20d29aaf23b25715e1be4a4` |
| Stage 4 | 1,473,920 | 1,338,374 | 206,790 | 15,340 / 71,336 | `0d1bbdd3d70c7a915ffb078ccebeba4c3f03116b579ce3a150f1e83a21f652fd` |
| Stage 5 | 1,511,664 | 1,371,812 | 215,942 | 16,008 / 76,192 | `b6212406cf5549ce47f69081915aeeacb967b88c444a2739a6c0351279d320b7` |
| CORE | 1,511,648 | 1,371,796 | 215,942 | 16,008 / 76,192 | `c3e37f5e28247e4a069807806c98f72cfe64ad90de2410f3d2f1891a283fa155` |
| CORE_SENSOR | 1,512,512 | 1,372,664 | 215,942 | 16,008 / 76,192 | `ac6f4f21d05035635341a27cf08d8ff640f2d4af5164035651394e25dc0fb485` |
| CORE_CLOUD | 1,512,176 | 1,372,320 | 215,942 | 16,008 / 76,192 | `231dd4a8ed67b329b24fec1d2f9fb1d6d5593bedeb25833860d73e61b4adfb28` |
| FULL | 1,512,896 | 1,373,048 | 215,942 | 16,008 / 76,192 | `496a4c31e30e8937c74188856e98085b64b74f291d079c66aa60ee45f7d4d21c` |

备份候选不常驻静态 RAM：上传 body、cJSON、原生候选和回滚快照按需从堆分配；候选 2 分钟后自动释放。`runtime_nvs` 事务快照位于 Flash。导出/验证/提交已记录内存事件，但 48 KiB 最坏输入、并发 Wi-Fi/Zigbee/RMT 和堆碎片仍需 C6 实测，不能仅凭静态 DIRAM 断言余量充足。
