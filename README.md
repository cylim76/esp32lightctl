# ESP32-C6 独立 Zigbee 智能照明网关

面向 `ESP32-C6-DevKitC-1-N8`、Aqara D1 单火三开 `QBKG25LM` 和 LG 红外空调的独立局域网网关。同一套源码可构建五个独立验证阶段；Stage 5 在 Stage 4 基础上增加 RMT 红外学习、重放和空调定时，不需要 Aqara 网关、Zigbee2MQTT、Home Assistant、MQTT 或常开 PC。

> 安全警告：QBKG25LM 直接连接市电。安装、拆卸和测量必须断电，并由具备资质的电工执行。ESP32 开发板不能与开关的市电端子有任何电气连接；两者只通过 Zigbee 通信。

## 可行性结论

- ESP32-C6 集成 2.4 GHz Wi-Fi 与 IEEE 802.15.4，ESP Zigbee SDK 自 0.5.4 起明确加入了 C6 单芯片 Wi-Fi/Zigbee gateway coexist 支持。本工程启用 `CONFIG_ESP_COEX_SW_COEXIST_ENABLE`，Wi-Fi 使用 `WIFI_PS_MIN_MODEM`，并调用 `esp_coex_wifi_i154_enable()`。
- 这不是“同时双接收”：C6 的两个协议共享射频和单天线，由软件仲裁分时使用。Espressif 对正式 Wi-Fi Zigbee 网关仍推荐 ESP32-S3 + ESP32-H2 双 SoC/双天线，以获得同时接收能力。因此本方案在功能上可行，但高 Wi-Fi 占空比、弱信号和拥挤 2.4 GHz 环境下的丢包率/配对率必须在现场验证。
- 固定版本为 **ESP-IDF 5.5.1 + esp-zigbee-lib 1.6.8 + esp-zboss-lib 1.6.4**。1.6.8 是官方仍维护的 1.x LTS，并明确基于 IDF 5.5.1；没有把 2026 年 2.x 新栈 API 与旧示例混用。
- 上游资料把 QBKG25LM 的 Zigbee model 标识为 `lumi.switch.l3acn3`，并通过标准 ZCL On/Off Cluster `0x0006` 控制。`state_left` 等只是 Zigbee2MQTT 的上层属性名，不是 Zigbee 命令。端点编号及其左/中/右关系必须由本项目的 Stage 3 在实机上采集，不能直接照搬上游映射。
- 经验证的开源转换器还会向 Aqara 厂商 Cluster `0xFCC0`、属性 `0x0009`（UINT8）、厂商码 `0x115F` 写入 `1`。Stage 3 只记录 Active Endpoints、Simple Descriptors 和 Cluster；Stage 4 也只有在用户根据实机日志明确确认三个不同的 On/Off server 端点后，才解锁控制和执行该初始化。

官方依据：

- [ESP Zigbee SDK 发布说明](https://github.com/espressif/esp-zigbee-sdk/blob/main/RELEASE_NOTES.md)
- [ESP-IDF ESP32-C6 RF coexistence](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-guides/coexist.html)
- [Espressif mDNS 组件文档](https://github.com/espressif/esp-protocols/blob/master/docs/mdns/en/index.rst)
- [QBKG25LM 转换器定义](https://github.com/Koenkk/zigbee-herdsman-converters/blob/master/src/devices/lumi.ts)
- [Aqara 自定义 Cluster/属性定义](https://github.com/Koenkk/zigbee-herdsman-converters/blob/master/src/lib/lumi.ts)
- [QBKG25LM 设备资料与配对步骤](https://www.zigbee2mqtt.io/devices/QBKG25LM.html)

## 已实现功能

- 首次启动生成随机 WPA2 配置热点；SSID 与一次性密码输出到 USB 串口。
- 移动端配置页写入 Wi-Fi SSID/密码和至少 12 位的管理密码，随后重启为 STA 模式。
- Wi-Fi 断线指数退避自动重连；串口打印连接状态和 IPv4 地址。
- STA 地址由 DHCP 获取；联网后发布可配置的 `http://<设备名>.local` mDNS 地址，默认 `http://lightgw.local`。页面同时显示 IPv4、STA MAC 和 RSSI，mDNS 不可用时可继续使用当前 IPv4。
- 管理员可从 Web 明确确认后进入安全重新配网：仅清除 Wi-Fi 凭据并重启到随机 WPA2 临时热点，保留原管理认证；配网完成即关闭热点。
- 管理密码使用随机盐 + PBKDF2-HMAC-SHA256（120,000 次）保存；控制页面及 API 使用 HTTP Basic 访问控制。
- Coordinator 建网、180 秒允许入网、设备 announce、Active Endpoint/Simple Descriptor/model 发现。
- Stage 3 的 QBKG25LM 诊断发现模式；Stage 4 的三路映射人工确认门禁、On/Off 控制、初始状态读取、属性上报和命令后回读。
- Web、调度器和后续云端入口统一经过 Device Control Service；每个命令拥有 ID，并区分 `PENDING`、`SENT`、`CONFIRMED`、`FAILED` 与实际状态 `UNKNOWN`。
- Zigbee 状态按 IEEE/短地址/端点隔离并记录更新时间；过期、回读超时、设备重新 announce 或短地址复用时，旧 OFF 不得确认本次关灯成功。
- Zigbee 栈网络数据保存在 `zb_storage` 分区；应用设备表、Wi-Fi 和定时设置保存在 NVS。
- 左/中/右、全部灯光、空调 1、空调 2、空调组各自最多 8 条计划；每条保存目标、启用状态、时分、星期、有限重试及补执行窗口。灯光使用目标端点的明确 OFF，空调只重放已确认保存的 OFF 学习码。
- DS3231 可选 RTC 使用原生 I²C 驱动，检查 OSF 后才恢复 UTC。系统时间一经 RTC、NTP 或管理员手动设置验证有效，不会因 Wi-Fi 离线而过期；每天北京时间 02:00 尝试 NTP，并在成功后回写 RTC。
- Web 时间管理区显示北京时间、UTC、来源、有效性、RTC/OSF、最近及下次 NTP；管理员可在无互联网时手动设置 ESP32 与在线的 DS3231。
- 默认定时关闭，启动/重启不会主动改变继电器状态。
- Stage 5 使用 ESP-IDF 原生 RMT：GPIO19 学习解调后的原始 mark/space，连续三次稳定后才允许确认；GPIO18 通过异步队列重放，支持两组关机码、可调载波/占空比和独立空调定时。
- 红外码存放于 Stage 5 专用 64KB NVS 分区。38kHz 是默认配置/估计值，不会被标记为接收头实测载波；页面严格区分“任务已提交”“RMT 已发送”和“空调实际状态未知”。
- Web 诊断页显示 Free Heap、Minimum Free Heap、Largest Free Block、每个 FreeRTOS 任务的 Stack High Water Mark，以及固定容量的关键内存事件；每 60 秒也会把资源摘要写入串口日志。

## 工程结构

```text
main/
  main.c                 初始化顺序
  stage_config.h         单源码 Stage 1～5 功能边界
  diagnostics.c          Heap、任务栈、关键内存事件、复位和最近错误诊断
  settings.c             NVS、PBKDF2 管理密码、定时配置
  wifi_manager.c         首次/重新配网、DHCP、mDNS、STA 重连
  rtc_manager.c          DS3231 I2C、UTC 读写和 OSF 检查
  time_manager.c         北京时间、NTP、RTC/手动时间来源和有效性
  web_server.c           鉴权 HTTP API
  zigbee_manager.c       Coordinator、发现、Aqara 协议、端点反馈
  zigbee_state_cache.c   多设备/端点隔离、有效期、回读超时
  device_control.c       Web/调度器共用的异步命令队列
  device_state.c         命令生命周期与结果历史
  scheduler.c            唯一自动关闭执行器、有限重试/补执行
  ir_receiver.c          RMT RX、长帧采集、LG 候选识别
  ir_transmitter.c       RMT TX、载波、原始码重放和安全限制
  ir_storage.c           两组红外码与空调定时的独立 NVS
  ir_manager.c           三次学习、带任务 ID 的异步发射与结果
  web/index.html         内置移动端页面
partitions.csv           NVS、应用、Zigbee 持久化、coredump
sdkconfig.defaults       C6、共存、Zigbee 默认配置
```

## 构建环境

建议从干净目录安装固定版本，不使用系统中其他 IDF：

### Windows

```bat
git clone --recursive --branch v5.5.1 https://github.com/espressif/esp-idf.git C:\esp\esp-idf-v5.5.1
cd C:\esp\esp-idf-v5.5.1
set IDF_GITHUB_ASSETS=dl.espressif.cn/github_assets
install.bat esp32c6
export.bat

cd D:\RPA\esp32lightctl
build-stage.bat 1
build-stage.bat 2
build-stage.bat 3
build-stage.bat 4
build-stage.bat 5
```

### Linux/macOS

```sh
git clone --recursive --branch v5.5.1 https://github.com/espressif/esp-idf.git ~/esp/esp-idf-v5.5.1
cd ~/esp/esp-idf-v5.5.1
./install.sh esp32c6
. ./export.sh

cd /path/to/esp32lightctl
idf.py -B build-stage1 -D LIGHTGW_STAGE=1 build
idf.py -B build-stage2 -D LIGHTGW_STAGE=2 build
idf.py -B build-stage3 -D LIGHTGW_STAGE=3 build
idf.py -B build-stage4 -D LIGHTGW_STAGE=4 build
idf.py -B build-stage5 -D LIGHTGW_STAGE=5 build
```

Component Manager 会严格解析 `main/idf_component.yml` 中的 `esp-zigbee-lib =1.6.8`、`esp-zboss-lib =1.6.4` 和官方 `mdns =1.14.1`。不要把约束改成 `*` 或直接用 SDK main 分支。

### 本地编译验证记录

2026-11-03 在 Windows 上使用 ESP-IDF 5.5.1、RISC-V GCC 14.2.0、`esp-zigbee-lib 1.6.8` 和 `esp-zboss-lib 1.6.4`，分别对 Stage 1～5 执行了真实 `idf.py build`。当前源码的构建结果、应用大小及 SHA-256 记录在 [BUILD.md](BUILD.md)；[firmware/SHA256SUMS.txt](firmware/SHA256SUMS.txt) 仅对应未改写的既有发布镜像。这些结果只证明编译与链接通过，不代表硬件测试通过。

Phase A 的控制边界见 [ARCHITECTURE.md](ARCHITECTURE.md) 和 [DEVICE_CONTROL.md](DEVICE_CONTROL.md)。可烧录 C 测试工程位于 `tests/phase_a_app`。

Stage 1～5 的 `idf.py size` / `size-components` 结果、运行时内存测试方法、Stage 6 RainMaker 前基线与 8 MB 双 OTA 候选规划见 [MEMORY_RESOURCE_REPORT.md](MEMORY_RESOURCE_REPORT.md)。可用 `memory-report.bat N` 重跑指定 Stage。静态构建结果不代表运行时内存余量已经通过硬件验证。

Flash/NVS 写入点、分区用途、非破坏性恢复策略和迁移风险见 [STORAGE_AUDIT.md](STORAGE_AUDIT.md)；本轮编译、QEMU C 测试、接口检查及未验证项见 [QUALITY_TEST_REPORT.md](QUALITY_TEST_REPORT.md)。开发修改后应运行 `check-all.bat`；它会执行源码存储策略检查、Web 接口检查、时间模型、JavaScript 解析、C6 测试固件构建以及 Stage 1～5 的 build/size/size-components。Node.js 不在 PATH 时通过 `NODE_EXE` 指定完整路径。

## USB 烧录与监控

将 DevKitC-1 的 USB 口连接电脑，先识别串口，再执行：

```bat
flash.bat COM7 1
monitor.bat COM7 1
```

依次验证其他 Stage 时只更改最后的阶段编号，例如：

```bat
flash.bat COM7 2
flash.bat COM7 3
flash.bat COM7 4
flash.bat COM7 5
```

Linux 示例：

```sh
idf.py -p /dev/ttyACM0 flash monitor
```

退出 monitor：`Ctrl+]`。若板子没有自动进入下载模式，按住 **BOOT**，短按 **RESET**，松开 **BOOT** 后重试。

彻底清除 Wi-Fi、管理密码、设备表和 Zigbee 网络（会要求所有 Zigbee 设备重新配对）：

```bat
idf.py -p COM7 erase-flash flash monitor
```

## 首次配置

1. 打开串口监视器，寻找类似日志：

   ```text
   W wifi: Provisioning AP 'LightGW-A1B2C3' started; one-time password: ...
   ```

2. 手机连接该热点，浏览器访问 `http://192.168.4.1/`。
3. 输入办公室 Wi-Fi、设备名称（默认 `lightgw`）、管理用户名和至少 12 位管理密码。设备名仅允许小写字母、数字和中间连字符，例如 `lightgw-office1`。固件不会在日志或状态 API 中回显 Wi-Fi 密码。
4. 保存后板子重启。串口出现 DHCP 地址和 `mDNS started` 日志，在同一局域网访问 `http://<设备名>.local`；若客户端或网络不支持 mDNS，则使用日志/页面显示的当前 IPv4。
5. 浏览器弹出认证框时输入刚设置的管理凭据。

如果办公室 Wi-Fi 使用开放认证、WPA-Enterprise/802.1X、强制门户或仅 5 GHz，本版 STA 配置不适用。当前阈值为 WPA2-PSK，WPA2/WPA3 personal mixed AP 可按现场结果调整。

办公室部署多台网关时必须分配不同设备名，例如 `lightgw-office1`、`lightgw-office2`。`.local` 解析依赖客户端 mDNS 支持、局域网组播和防火墙策略；解析失败不是 HTTP 服务失效，应改用页面或串口显示的 DHCP IPv4。

## QBKG25LM 配对与测试

1. 先按 [TEST_PLAN.md](TEST_PLAN.md) 完成 Stage 1 和 Stage 2；Stage 3 中建议让 Wi-Fi 和 NTP 稳定，并确认页面显示 Zigbee PAN、信道 15。
2. 点击“开放配对 180 秒”。
3. 在 QBKG25LM 上长按按键约 10 秒，直到蓝灯闪烁后停止，松手等待。如果它仍属于旧网，必须先从旧 Coordinator 删除或执行设备复位。
4. 串口应依次看到：

   ```text
   Device announce short=0x....
   DIAG short=0x.... active_endpoint=...
   DIAG short=0x.... ep=... input_cluster=0x0006
   QBKG25LM model found at 0x....; control remains locked until physical channel mapping is confirmed
   ```

5. 保存 Stage 3 完整日志，确认三个 On/Off server 端点及其与左/中/右继电器的实际对应关系。仅在 Stage 4 页面再次输入并明确确认该映射后，开关控件才会解锁。
6. 用实体按键切换三路，确认页面在数秒内随 `0x0006/OnOff` 属性报告更新。
7. 断开 ESP32 USB 电源 30 秒再上电：确认 PAN ID/设备仍在、不自动改变灯状态，并能重新读取三路状态。
8. 配置多条关灯时间（例如每天 17:00、19:30、20:30、21:30、23:59），逐条确认当天仅执行一次；获得可信时间后断开办公室网络，确认计划继续按北京时间执行。没有可信 RTC/NTP/手动时间时必须跳过。
9. 连续执行至少 200 次三路切换，同时对网页做刷新/局域网 ping；记录命令超时、Zigbee 重试、Wi-Fi 断线和 coredump。

## 无线共存验证建议

- 本工程默认 Zigbee 信道 15（`main/app_config.h`）。部署前用 Wi-Fi 扫描和 802.15.4 抓包确认现场干扰；常见做法是在 Wi-Fi 信道确定后选择重叠较少的 Zigbee 15、20 或 25。
- Zigbee 网络形成后不要只修改固件信道常量；已有设备不会自动迁移。测试阶段可 `erase-flash` 后重建网络。
- 在办公室 AP 满负载下载、空闲、弱信号三个条件下分别测试入网和开关延迟。单芯片/单天线无法保证与双芯片网关相同的并发接收性能。
- 若出现大量 `NO_ACK`、入网失败或实体按键上报丢失：先固定 Wi-Fi 2.4 GHz 信道、降低 Wi-Fi 流量、调整 Zigbee 信道和天线位置；仍不满足指标时，硬件层面的可靠方案是双 SoC/双天线，而不是在应用层无限重试。

## 安全边界

- 随机配置热点密码只通过 USB 串口显示；管理密码只保存 PBKDF2 派生值。
- 当前页面是 HTTP Basic **访问控制**，不是 TLS。它能阻止不知道凭据的普通局域网用户操作，但同网段主动嗅探者仍可能获得凭据。生产网络应使用受信 VLAN/访客隔离，或在前端增加 HTTPS/反向代理（这会违反“无外部服务器”的硬件限制）。
- 默认开发构建的 NVS 没有静态加密，Wi-Fi 密码会由 ESP-IDF Wi-Fi/NVS 组件保存在 flash。若“安全保存”包含对拆机读取 flash 的防护，量产前必须启用 ESP32-C6 Flash Encryption，并按 Espressif 的量产流程保护/烧录 eFuse 密钥。该过程可能不可逆，本工程不会在普通 `flash` 命令中擅自烧 eFuse。
- 不要把管理密码、Wi-Fi 密码或量产密钥提交到源码、`sdkconfig.defaults` 或日志。

## 已验证与未验证边界

本仓库只会把成功完成的本机构建记录为“编译验证”。以下行为必须有实体 `ESP32-C6-DevKitC-1-N8 + QBKG25LM + 办公室 AP` 才能确认，不能由编译代替：

- QBKG25LM 不同固件批次是否都接受 `0xFCC0/0x0009=1`；
- 三路实体按键是否每次产生标准 On/Off 属性报告；
- 单火负载类型、最小功率、防闪烁模式和断电恢复表现；
- 现场 RF 环境中的长时间共存可靠性；
- NTP/DNS/防火墙策略与办公室 Wi-Fi 的兼容性。
- 客户端、交换机/AP 和防火墙对 mDNS 组播的支持，以及断线重连、DHCP 地址变化后的解析更新。

遇到问题请保留从复位开始的完整 `idf.py monitor` 日志，尤其是 Zigbee announce、Simple Descriptor、read/report response、Wi-Fi disconnect reason 和 backtrace。

Stage 5 的低压接线、电源边界和红外硬件验证分别见 [IR_WIRING.md](IR_WIRING.md) 与 [IR_TEST_PLAN.md](IR_TEST_PLAN.md)。红外模块未经实物验证；LG 协议识别结果只是候选判断，最终必须以学习码重放和空调实际响应为准。

DS3231 接线、电平风险、UTC/北京时间规则及手动设置流程见 [RTC_TIME.md](RTC_TIME.md)。不同成品模块的上拉和充电电路并不相同，连接前必须检查，RTC 功能目前仅完成软件构建与模拟验证。

统一硬件验证状态见 [HARDWARE_TEST_RECORD.md](HARDWARE_TEST_RECORD.md)。开发板到货前全部硬件测试均保持 `NOT TESTED`，不得因固件成功生成而改成 `PASS`。
