# 硬件验证计划

所有硬件结果统一填写到 `HARDWARE_TEST_RECORD.md`。只允许 `PASS`、`FAIL`、`NOT TESTED`；`idf.py build` 成功不能填写为硬件 PASS。

## 推荐顺序

1. Stage 1：只验证供电、串口、首次配网、Web、鉴权、NVS、Wi-Fi 重连和诊断指标。
2. Stage 2：确认 Stage 1 无回归后验证单芯片 Wi-Fi/Zigbee 共存、Coordinator 建网与 180 秒 Permit Join。
3. Stage 3：复位 QBKG25LM 并配对，只记录实际 Active Endpoints、输入/输出 Cluster、model 和状态报文，不控制市电负载。
4. 根据串口 `DIAG` 和实际逐路实验确定映射。三路端点未确认前禁止点击映射确认。
5. Stage 4：保存已验证映射后，分别测试左/中/右、实体按键反馈、全关、定时、掉线恢复、断电恢复与长稳。

## Stage 1

- 首次启动显示随机 WPA2 配置热点和一次性密码。
- 手机连接 `192.168.4.1`，保存 Wi-Fi 与至少 12 位管理密码。
- 重启后 STA 获得 IP，页面需要 HTTP Basic 认证。
- 页面显示 RSSI、IP、运行时间、Free Heap、Minimum Free Heap、复位原因和最近错误。
- 页面显示 STA MAC、设备名称和 mDNS 主机名；验证 `http://lightgw.local`（或配置名）与当前 IPv4 均可访问。
- 从管理页触发重新配网，确认未认证请求被拒绝、旧 Wi-Fi 凭据被清除、随机 WPA2 热点启动且管理认证仍然有效。
- 将设备名改为 `lightgw-office1` 后确认 DHCP hostname、mDNS 地址和页面显示一致。
- Wi-Fi 断开、重连及 DHCP 地址变化后重新验证 mDNS 与 IPv4 访问。
- 连续断开/恢复 AP 三次，记录断线 reason 和重连时间。
- 断电 30 秒后配置仍存在，启动不改变任何灯具状态。

## Stage 2

- 串口显示 Stage 2、Coordinator 建网或恢复、PAN ID、信道。
- Web 开放 Permit Join 180 秒，页面倒计时与串口状态一致。
- Wi-Fi 持续 ping 与 HTTP 刷新时，观察 Zigbee 建网/Permit Join 是否异常。
- 分别在 Wi-Fi 空闲、高吞吐、弱 RSSI 条件记录 Free Heap、Minimum Free Heap、断线和 Zigbee 错误。

## Stage 3

- QBKG25LM 退出旧网络并复位，再执行配对。
- 保存从 `Device announce` 开始的完整日志。
- 记录所有 `active_endpoint`、`input_cluster`、`output_cluster` 和 model。
- 与实体左/中/右动作逐一对应，不能根据 Zigbee2MQTT 的 `state_left` 名称推断底层端点。
- 页面只能显示发现记录，控制按钮必须保持锁定。

## Stage 4

- 仅把 Stage 3 已实测的三个 On/Off server endpoint 保存为左/中/右映射。
- 每路执行开/关各 20 次，核对物理继电器和状态反馈。
- 实体按键每路 20 次，页面状态应更新。
- 全部关闭、星期掩码、NTP 同步前不执行、断网跨过计划时间不执行且不补发。
- Wi-Fi 断线/恢复、Zigbee 设备掉电/恢复、ESP32 断电/恢复。
- Wi-Fi 高吞吐下执行不少于 200 次 Zigbee 操作。
- 连续运行至少 72 小时，记录重启、coredump、Minimum Free Heap 趋势、丢包和最大响应时间。

## 安全

QBKG25LM 连接市电，安装、拆卸和测量必须断电并由合格电工执行。ESP32 与开关之间只能通过 Zigbee 通信，禁止任何市电端子与开发板电气连接。
