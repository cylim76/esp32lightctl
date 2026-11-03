# 构建说明与软件验证记录

## 固定工具链

- ESP-IDF `v5.5.1`
- ESP Zigbee SDK `esp-zigbee-lib 1.6.8`
- ZBOSS `esp-zboss-lib 1.6.4`
- Espressif mDNS `1.14.1`
- 目标：`esp32c6`，Flash：8 MB
- 固件版本：`0.3.0-stage1` ～ `0.3.0-stage4`

四个 Stage 使用同一套源码。`LIGHTGW_STAGE` 是 CMake cache 变量，每个 Stage 使用独立的 `build-stageN` 目录和该目录内独立生成的 `sdkconfig`。

## Windows 环境

```bat
cd C:\esp\esp-idf-v5.5.1
export.bat
cd D:\RPA\esp32lightctl
build-stage.bat 1
build-stage.bat 2
build-stage.bat 3
build-stage.bat 4
```

等价的原生命令：

```bat
idf.py -B build-stage1 -D LIGHTGW_STAGE=1 build
idf.py -B build-stage2 -D LIGHTGW_STAGE=2 build
idf.py -B build-stage3 -D LIGHTGW_STAGE=3 build
idf.py -B build-stage4 -D LIGHTGW_STAGE=4 build
```

不带参数执行 `idf.py build` 时默认构建 Stage 4，但推荐始终显式指定独立目录。

## Stage 边界

| Stage | 固件包含内容 | 软件构建可证明 | 必须上板验证 |
|---|---|---|---|
| 1 | NVS、首次/重新配网、DHCP、mDNS、Wi-Fi STA、NTP、Web、访问控制、诊断 | 条件编译和链接成功 | AP/STA、mDNS、页面、NVS、重连 |
| 2 | Stage 1 + Coordinator、建网、Permit Join、共存启用 | Zigbee API 与 Wi-Fi 共存配置可链接 | RF 共存、建网、允许入网 |
| 3 | Stage 2 + Active Endpoint、Simple Descriptor、Cluster/model 诊断 | 诊断路径可链接 | QBKG25LM 实际端点和 Cluster |
| 4 | Stage 3 + 映射确认后的控制、状态、调度器 | 完整集成路径可链接 | 三路映射、反馈、定时、长期稳定性 |

Stage 3 不会启用三路控制。Stage 4 也要求先在页面明确保存现场验证过的左/中/右端点，才会解锁控制；重新发现设备会再次锁定映射。

## 2026-11-03 真实构建结果

以下状态仅代表 `idf.py build` 成功，不代表硬件测试通过。最终哈希记录在提交时生成。

| Stage | 结果 | 应用大小 | 应用 SHA-256 |
|---|---|---:|---|
| 1 | PASS (BUILD ONLY) | 1,009,776 B (`0xf6870`) | `b64e2a44075b649e4196629f4b3245933b62525404c92210b0ffbaa29bafa424` |
| 2 | PASS (BUILD ONLY) | 1,368,400 B (`0x14e150`) | `a0a213ad5a87842625b9b4b0883545bbfce11324b070f3608f0031d39cc1bf60` |
| 3 | PASS (BUILD ONLY) | 1,371,136 B (`0x14ec00`) | `710c11c2bf8c1901f0695396df80c2016b64d91766a30477cd29e9ea47046b56` |
| 4 | PASS (BUILD ONLY) | 1,380,096 B (`0x150f00`) | `398bcf8c1eba67d455d7cd96dfee1b8ef1b538322e7de747f2610dca21295814` |

所有 Stage 使用相同分区表。烧录地址为：bootloader `0x0`、partition table `0x8000`、factory app `0x10000`。

可直接烧录的镜像位于 `firmware/stage1` ～ `firmware/stage4`，完整 bootloader/partition/app 哈希见 `firmware/SHA256SUMS.txt`。bootloader 内含构建时间元数据，因此各 Stage 的 bootloader 哈希可能不同；分区表内容相同。
