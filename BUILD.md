# 构建说明与软件验证记录

## 固定工具链

- ESP-IDF `v5.5.1`
- ESP Zigbee SDK `esp-zigbee-lib 1.6.8`
- ZBOSS `esp-zboss-lib 1.6.4`
- Espressif mDNS `1.14.1`
- 目标：`esp32c6`，Flash：8 MB
- 当前源码固件版本：`0.7.0-stageN`；原 Stage 1～5 已发布镜像保持不变

五个 Stage 使用同一套源码。`LIGHTGW_STAGE` 是 CMake cache 变量，每个 Stage 使用独立的 `build-stageN` 目录和该目录内独立生成的 `sdkconfig`。Stage 5 叠加 `sdkconfig.stage5` 并使用 `partitions_stage5.csv`；Stage 1～4 继续使用原 `partitions.csv`。

## Windows 环境

```bat
cd C:\esp\esp-idf-v5.5.1
export.bat
cd D:\RPA\esp32lightctl
build-stage.bat 1
build-stage.bat 2
build-stage.bat 3
build-stage.bat 4
build-stage.bat 5
```

等价的原生命令：

```bat
idf.py -B build-stage1 -D LIGHTGW_STAGE=1 build
idf.py -B build-stage2 -D LIGHTGW_STAGE=2 build
idf.py -B build-stage3 -D LIGHTGW_STAGE=3 build
idf.py -B build-stage4 -D LIGHTGW_STAGE=4 build
idf.py -B build-stage5 -D LIGHTGW_STAGE=5 build
```

不带参数执行 `idf.py build` 时默认构建 Stage 4，但推荐始终显式指定独立目录。

## Stage 边界

| Stage | 固件包含内容 | 软件构建可证明 | 必须上板验证 |
|---|---|---|---|
| 1 | NVS、首次/重新配网、DHCP、mDNS、Wi-Fi STA、NTP、Web、访问控制、诊断 | 条件编译和链接成功 | AP/STA、mDNS、页面、NVS、重连 |
| 2 | Stage 1 + Coordinator、建网、Permit Join、共存启用 | Zigbee API 与 Wi-Fi 共存配置可链接 | RF 共存、建网、允许入网 |
| 3 | Stage 2 + Active Endpoint、Simple Descriptor、Cluster/model 诊断 | 诊断路径可链接 | QBKG25LM 实际端点和 Cluster |
| 4 | Stage 3 + 映射确认后的控制、状态、调度器 | 完整集成路径可链接 | 三路映射、反馈、定时、长期稳定性 |
| 5 | Stage 4 + RMT RX/TX、两组红外码、独立 NVS、Web 与空调调度 | RMT API、存储、队列和完整集成可编译链接 | GPIO、电源、LG OFF 学习/重放、两台覆盖与共存 |

Stage 3 不会启用三路控制。Stage 4 也要求先在页面明确保存现场验证过的左/中/右端点，才会解锁控制；重新发现设备会再次锁定映射。

## 2026-11-03 真实构建结果

下表为 `0.4.0` 多时段调度版本的真实构建记录。任何构建结果都不代表硬件测试通过。

| Stage | 结果 | 应用大小 | 应用 SHA-256 |
|---|---|---:|---|
| 1 | PASS (BUILD ONLY) | 1,011,488 B (`0xf6f20`) | `aabd1c393d95422886a234faebe81105c8c6e35bf1c8ad934a9bb9227728fd27` |
| 2 | PASS (BUILD ONLY) | 1,370,080 B (`0x14e7e0`) | `57b706ca290158e81fc8ade896102ac71138b4198fe67f5206070aa7a464f81c` |
| 3 | PASS (BUILD ONLY) | 1,372,832 B (`0x14f2a0`) | `1dd589c476dde66ae8b60102b25088e0b4ef3adebec54f16d40145b0da65a622` |
| 4 | PASS (BUILD ONLY) | 1,382,544 B (`0x151890`) | `506c3e3e725eb5d6297612131953d8b7557912ab4f7fe0fa23a88136ac17c672` |

Stage 1～4 继续使用原分区表且已发布固件不修改。Stage 5 保持原 NVS、factory、Zigbee 和 coredump 的地址不变，在 coredump 后新增 64KB `ir_data` NVS。所有 Stage 烧录地址仍为：bootloader `0x0`、partition table `0x8000`、factory app `0x10000`。

原有镜像位于 `firmware/stage1` ～ `firmware/stage4`，本次开发不会改写这些文件。Stage 5 最终镜像单独放在 `firmware/stage5`，完整哈希见 `firmware/SHA256SUMS.txt`。bootloader 内含构建时间元数据，因此各 Stage 的 bootloader 哈希可能不同。

## 2026-11-03 Stage 5 最终回归构建

在加入 Stage 5 后，使用同一套源码和五个独立 build 目录依次执行 `build-stage.bat 1` 至 `build-stage.bat 5`，五次命令均以退出码 0 完成。Stage 1～4 下表哈希是本次源码回归产生的临时 build 输出；为遵守发布文件不变要求，没有覆盖 `firmware/stage1` ～ `firmware/stage4` 中已发布的镜像。

| Stage | 结果 | 应用大小 | 本次回归应用 SHA-256 |
|---|---|---:|---|
| 1 | PASS (BUILD ONLY) | 1,016,560 B (`0xf82f0`) | `28e77da7d3b2ae2023dc769e5adc5e9810a0679db10ddbca3edcfa9f57ff42db` |
| 2 | PASS (BUILD ONLY) | 1,375,168 B (`0x14fbc0`) | `c2abfb83fe4d3975f3c159b115d664fdcdcbb44db34eb93b288893ada42a82bd` |
| 3 | PASS (BUILD ONLY) | 1,377,904 B (`0x150670`) | `e908747f62a6c42b987b94d171bd997293d50f7a5b16d4d8947d6cbfb2f64cfe` |
| 4 | PASS (BUILD ONLY) | 1,387,632 B (`0x152c70`) | `0c7cdf7e9a818787b20fba3a6efa07a52f80358afe81679c2cebb2b2d8e6c39e` |
| 5 | PASS (BUILD ONLY) | 1,426,352 B (`0x15c3b0`) | `683a732d8467a2e18babc439c0e067b392b325e290ffbd681d220f46cebc53c6` |

Stage 5 最小应用分区为 3,145,728 B（`0x300000`），应用镜像剩余 1,719,376 B（约 55%）。红外收发、空调响应、电源稳定性以及 Wi-Fi/Zigbee/RMT 实际共存仍全部为 `NOT TESTED`。

## 2026-11-03 RTC 与按设备调度版本回归构建

版本 `0.6.0-stageN` 使用 ESP-IDF 5.5.1 对五个独立构建目录执行真实构建，全部退出码为 0。下列哈希对应本地 `build-stageN/esp32c6_light_gateway.bin`；没有覆盖 `firmware/` 中已发布镜像。

| Stage | 结果 | 应用大小 | 应用 SHA-256 |
|---|---|---:|---|
| 1 | PASS (BUILD ONLY) | 1,063,488 B (`0x103a40`) | `3d85b8ce7ab9ce2680b63807e0baca20b5d6ffb8f143bc4a730f0014f69a69f4` |
| 2 | PASS (BUILD ONLY) | 1,422,192 B (`0x15b370`) | `b946b416efcbeb2b1b252520d57178a4b2b5c568360a9c392efaff03a054f118` |
| 3 | PASS (BUILD ONLY) | 1,424,928 B (`0x15be20`) | `63863a04344e5b2fcbf65770767e0ba2abdd60b6ed35e78b50aa92b960665f33` |
| 4 | PASS (BUILD ONLY) | 1,434,896 B (`0x15e510`) | `2d768efb3ff2daf6d02904c8dd3dfc9f647cdf804320073cebb31b100f8a0004` |
| 5 | PASS (BUILD ONLY) | 1,470,848 B (`0x167180`) | `c1a28e4cdccdb67f52dfd4402ff1ec843efd1f17438fcb77d2385bd903a5c75a` |

同时执行 `tests/check_web_js.js` 及 `tests/simulate_time_scheduler.py`，网页脚本解析和 A～L 状态机情景均为 `SIMULATED PASS`。这些结果不是开发板、DS3231、QBKG25LM 或 LG 空调的硬件 PASS。

## 2026-11-03 Phase A 统一控制回归

版本 `0.7.0-stageN` 使用同一固定工具链真实执行五次 `idf.py build`，均以退出码 0 完成。哈希对应本地构建目录，没有改写 `firmware/` 中既有 Stage 1～5 发布镜像。

| Stage | 结果 | 应用大小 | 应用 SHA-256 |
|---|---|---:|---|
| 1 | PASS (BUILD ONLY) | 1,062,576 B (`0x1036b0`) | `911171a54a6bdecf141570cad366a8f8da30771809de4da15bf0f82cb01b06ae` |
| 2 | PASS (BUILD ONLY) | 1,423,328 B (`0x15b7e0`) | `d13746218b3af66389462580666dcf5e0270fec87bba358b127f1fe4f1f32a7e` |
| 3 | PASS (BUILD ONLY) | 1,426,624 B (`0x15c4c0`) | `da7719daeeb64fd76dc824ccbbe2dedba063a519728739b5f2a1a694fafe0557` |
| 4 | PASS (BUILD ONLY) | 1,440,144 B (`0x15f990`) | `3a8f56faff641ca1f173346383d27591ae445d77521a350656397e97e7411263` |
| 5 | PASS (BUILD ONLY) | 1,476,016 B (`0x1685b0`) | `b0246b04361c1f0d8ac7ec0d883858e23e8aa20a98ac14305173d75157990f35` |

另外对 `tests/phase_a_app` 执行真实 ESP-IDF 构建。它直接包含生产 C 状态缓存/命令状态模块；ESP32-C6 测试镜像为 111,680 B（`0x1b440`），SHA-256 `fae55b96d6538466c4aff0b00376a96f77c027a66e1c2e955318bda342ed1288`。8 项测试还使用 `sdkconfig.qemu` 构建为 ESP32-C3，并在 ESP-IDF 官方 QEMU 9.0.0 中实际执行，全部输出 `PASS`，结果记为 `SIMULATED PASS`。C6 目标板尚未烧录运行，硬件状态仍为 `NOT TESTED`。

QEMU 命令需要先安装 ESP-IDF 可选的 `qemu-riscv32` 工具，然后使用独立 `build-qemu` 目录；该模拟只验证与芯片无关的 C 状态逻辑，不验证 C6 外设或无线行为。

软件侧另有：网页内联 JavaScript 解析 `SIMULATED PASS`；时间/调度 A～L、每目标 8 条/总计 56 条、v2 迁移模型均为 `SIMULATED PASS`。所有 Zigbee、RMT、DS3231 和 RF 实物行为仍为 `NOT TESTED`。
