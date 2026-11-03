# ESP32-C6 独立 Zigbee 智能照明网关

面向 `ESP32-C6-DevKitC-1-N8` 与 Aqara D1 单火三开 `QBKG25LM` 的独立局域网网关。固件在一块 C6 上同时运行 Wi-Fi STA、Zigbee Coordinator、HTTP 控制页、NTP 和本地定时任务；不需要 Aqara 网关、Zigbee2MQTT、Home Assistant、MQTT 或常开 PC。

> 安全警告：QBKG25LM 直接连接市电。安装、拆卸和测量必须断电，并由具备资质的电工执行。ESP32 开发板不能与开关的市电端子有任何电气连接；两者只通过 Zigbee 通信。

## 可行性结论

- ESP32-C6 集成 2.4 GHz Wi-Fi 与 IEEE 802.15.4，ESP Zigbee SDK 自 0.5.4 起明确加入了 C6 单芯片 Wi-Fi/Zigbee gateway coexist 支持。本工程启用 `CONFIG_ESP_COEX_SW_COEXIST_ENABLE`，Wi-Fi 使用 `WIFI_PS_MIN_MODEM`，并调用 `esp_coex_wifi_i154_enable()`。
- 这不是“同时双接收”：C6 的两个协议共享射频和单天线，由软件仲裁分时使用。Espressif 对正式 Wi-Fi Zigbee 网关仍推荐 ESP32-S3 + ESP32-H2 双 SoC/双天线，以获得同时接收能力。因此本方案在功能上可行，但高 Wi-Fi 占空比、弱信号和拥挤 2.4 GHz 环境下的丢包率/配对率必须在现场验证。
- 固定版本为 **ESP-IDF 5.5.1 + esp-zigbee-lib 1.6.8 + esp-zboss-lib 1.6.4**。1.6.8 是官方仍维护的 1.x LTS，并明确基于 IDF 5.5.1；没有把 2026 年 2.x 新栈 API 与旧示例混用。
- QBKG25LM 的 Zigbee model 是 `lumi.switch.l3acn3`。开关控制使用标准 ZCL On/Off Cluster `0x0006`，端点 1/2/3 分别对应左/中/右。`state_left` 等只是 Zigbee2MQTT 的上层属性名，不是 Zigbee 命令。
- 经验证的开源转换器还会向 Aqara 厂商 Cluster `0xFCC0`、属性 `0x0009`（UINT8）、厂商码 `0x115F` 写入 `1`。本工程复用了这一初始化，并先读取 Active Endpoints/Simple Descriptors，确认端点确实提供 `0x0006` server 后才允许 Web 控制。

官方依据：

- [ESP Zigbee SDK 发布说明](https://github.com/espressif/esp-zigbee-sdk/blob/main/RELEASE_NOTES.md)
- [ESP-IDF ESP32-C6 RF coexistence](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-guides/coexist.html)
- [QBKG25LM 转换器定义](https://github.com/Koenkk/zigbee-herdsman-converters/blob/master/src/devices/lumi.ts)
- [Aqara 自定义 Cluster/属性定义](https://github.com/Koenkk/zigbee-herdsman-converters/blob/master/src/lib/lumi.ts)
- [QBKG25LM 设备资料与配对步骤](https://www.zigbee2mqtt.io/devices/QBKG25LM.html)

## 已实现功能

- 首次启动生成随机 WPA2 配置热点；SSID 与一次性密码输出到 USB 串口。
- 移动端配置页写入 Wi-Fi SSID/密码和至少 12 位的管理密码，随后重启为 STA 模式。
- Wi-Fi 断线指数退避自动重连；串口打印连接状态和 IPv4 地址。
- 管理密码使用随机盐 + PBKDF2-HMAC-SHA256（120,000 次）保存；控制页面及 API 使用 HTTP Basic 访问控制。
- Coordinator 建网、180 秒允许入网、设备 announce、Active Endpoint/Simple Descriptor/model 发现。
- QBKG25LM 端点 1/2/3 的 On/Off 控制、初始状态读取、属性上报和命令后回读。
- Zigbee 栈网络数据保存在 `zb_storage` 分区；应用设备表、Wi-Fi 和定时设置保存在 NVS。
- 每日按星期自动全部关灯；仅在 Wi-Fi 在线并在本次连接中 NTP 同步成功后执行。断网时跳过，不补发。
- 默认定时关闭，启动/重启不会主动改变继电器状态。

## 工程结构

```text
main/
  main.c                 初始化顺序
  settings.c             NVS、PBKDF2 管理密码、定时配置
  wifi_manager.c         首次配网、STA 重连、NTP
  web_server.c           鉴权 HTTP API
  zigbee_manager.c       Coordinator、发现、Aqara 协议、状态同步
  scheduler.c            每周定时自动关闭
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
idf.py set-target esp32c6
idf.py build
```

### Linux/macOS

```sh
git clone --recursive --branch v5.5.1 https://github.com/espressif/esp-idf.git ~/esp/esp-idf-v5.5.1
cd ~/esp/esp-idf-v5.5.1
./install.sh esp32c6
. ./export.sh

cd /path/to/esp32lightctl
idf.py set-target esp32c6
idf.py build
```

Component Manager 会严格解析 `main/idf_component.yml` 中的 `esp-zigbee-lib =1.6.8` 和 `esp-zboss-lib =1.6.4`。不要把约束改成 `*` 或直接用 SDK main 分支。

### 本地编译验证记录

2026-11-03 在 Windows 上使用 ESP-IDF 5.5.1、RISC-V GCC 14.2.0、`esp-zigbee-lib 1.6.8` 和 `esp-zboss-lib 1.6.4` 完成了最终集成构建：

```bat
idf.py build
```

构建结果：

- `bootloader.bin`：`0x57e0` 字节，bootloader 分区剩余 31%；
- `esp32c6_light_gateway.bin`：`0x142b20` 字节（约 1.26 MiB）；
- factory 应用分区：3 MiB，剩余 `0x1bd4e0` 字节（58%）；
- 分区表、内置网页、Wi-Fi/NTP/NVS/HTTP、调度器、Coordinator、设备发现及三路控制源码均参与同一次编译和最终链接。

当前 `build` 目录产物 SHA-256：

```text
esp32c6_light_gateway.bin  c57f023472959a49933992ee236aa9d45f8477cec3efe748cb95d320d281af3c
bootloader.bin             e911315ab508cf6d9cfb64b51b23c3e4d09239e63705ad379be43c22f62175e8
partition-table.bin        9087e0ba8008ce05983c896735c945801f3c34d4e4038528ddd619116765659b
```

四个开发阶段的源码都已纳入上述最终集成构建；本工作区没有伪造四次独立的历史构建或硬件测试记录。使用完整的 `git clone --recursive` IDF 环境时，普通 `idf.py build` 即可得到等价结果。

## USB 烧录与监控

将 DevKitC-1 的 USB 口连接电脑，先识别串口，再执行：

```bat
idf.py -p COM7 flash monitor
```

若直接使用本工作区已经验证的 `build_min2` 产物：

```bat
idf.py -B build_min2 -p COM7 flash monitor
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
3. 输入办公室 Wi-Fi、管理用户名和至少 12 位管理密码。固件不会在日志或状态 API 中回显 Wi-Fi 密码。
4. 保存后板子重启。串口出现 `Wi-Fi connected, IP: ...`，在同一局域网访问该 IP。
5. 浏览器弹出认证框时输入刚设置的管理凭据。

如果办公室 Wi-Fi 使用开放认证、WPA-Enterprise/802.1X、强制门户或仅 5 GHz，本版 STA 配置不适用。当前阈值为 WPA2-PSK，WPA2/WPA3 personal mixed AP 可按现场结果调整。

## QBKG25LM 配对与测试

1. 建议先让 Wi-Fi 和 NTP 稳定；确认页面显示 Zigbee PAN、信道 15。
2. 点击“开放配对 180 秒”。
3. 在 QBKG25LM 上长按按键约 10 秒，直到蓝灯闪烁后停止，松手等待。如果它仍属于旧网，必须先从旧 Coordinator 删除或执行设备复位。
4. 串口应依次看到：

   ```text
   Device announce short=0x....
   0x.... endpoint 1 exposes On/Off server cluster
   0x.... endpoint 2 exposes On/Off server cluster
   0x.... endpoint 3 exposes On/Off server cluster
   Supported QBKG25LM discovered at 0x....
   ```

5. 在页面分别切换左、中、右，现场确认对应继电器，不只看网页状态。
6. 用实体按键切换三路，确认页面在数秒内随 `0x0006/OnOff` 属性报告更新。
7. 断开 ESP32 USB 电源 30 秒再上电：确认 PAN ID/设备仍在、不自动改变灯状态，并能重新读取三路状态。
8. 断开办公室网络后跨过定时时刻：确认定时任务不执行；恢复网络并重新 NTP 同步后测试下一次定时。
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

遇到问题请保留从复位开始的完整 `idf.py monitor` 日志，尤其是 Zigbee announce、Simple Descriptor、read/report response、Wi-Fi disconnect reason 和 backtrace。
