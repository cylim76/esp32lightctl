# 硬件测试记录

硬件：ESP32-C6-DevKitC-1-N8 + Aqara QBKG25LM + 红外 RX/TX 模块 + LG 空调。以下未执行项目全部为 `NOT TESTED`。构建成功只记录在 `BUILD.md`。

| ID | Stage | 测试项 | 状态 | 日期/固件 | 证据或备注 |
|---|---:|---|---|---|---|
| HW-001 | 1 | USB 启动与串口日志 | PASS | 2026-11-06 / Stage 1 | J5 供电跳线修复后 COM8 可烧录、启动和持续监视 |
| HW-002 | 1 | 首次配置热点与 Web 配网 | PASS | 2026-11-06 / Stage 1 | 已完成临时热点配网并连接 MiFi-0300；后续按部署要求改为固定临时热点密码 admin123，尚未重新配网复测 |
| HW-003 | 1 | Wi-Fi STA、IP、NTP、RSSI | PASS | 2026-11-06 / Stage 1 | DHCP 192.168.1.197；NTP 获得有效北京时间；资源日志持续正常 |
| HW-004 | 1 | 表单登录、RAM 会话及未授权访问拦截 | PASS | 2026-11-06 / Stage 1 | 旧凭据首次登录成功，随后多次刷新复用 RAM 会话且未再触发 WDT；复位后要求重新登录符合设计 |
| HW-005 | 1 | NVS 断电持久化 | PASS | 2026-11-06 / Stage 1 | 多次应用刷写/复位后 Wi-Fi、设备名和管理员配置均恢复，未擦除 NVS |
| HW-006 | 1 | Wi-Fi 断线自动重连 | NOT TESTED | — | 等待硬件 |
| HW-007 | 1 | DHCP IPv4、STA MAC 与 RSSI 显示 | NOT TESTED | — | 等待硬件 |
| HW-008 | 1 | 默认及自定义主机名 mDNS 解析 | FAIL | 2026-11-06 / Stage 1 | 固件成功发布 lightgw.local，但当前客户端/网络无法解析；IPv4 192.168.1.197 可正常访问，需继续排查客户端 mDNS/组播/防火墙 |
| HW-009 | 1 | 断线/重连/IP 变化后的 mDNS 更新 | NOT TESTED | — | 等待硬件 |
| HW-010 | 1 | 管理员认证重新配网与临时 WPA2 AP | NOT TESTED | — | 等待硬件 |
| HW-011 | 1 | DS3231 3.3V 电平、I2C GPIO6/7 与 OSF | NOT TESTED | — | 先检查成品模块上拉/充电电路 |
| HW-012 | 1 | 无网络管理员手动设置 ESP32 与 DS3231 | NOT TESTED | — | 页面使用北京时间，RTC 存 UTC |
| HW-013 | 1 | RTC 断电恢复、NTP 02:00 同步及离线长期计时 | NOT TESTED | — | 构建/模拟通过不等于硬件通过 |
| HW-101 | 2 | Coordinator 首次建网与恢复 | PASS | 2026-11-06 / Stage 2 | 修复 SKIP_STARTUP 信号处理后恢复 PAN 0xDD9F、信道15；首次建网路径仍需在擦除测试网络后单独复测 |
| HW-102 | 2 | Permit Join 180 秒 | PASS | 2026-11-06 / Stage 2 | 串口记录请求、status=180，并在超时后自动 status=0 |
| HW-103 | 2 | Wi-Fi/Zigbee 单芯片 RF 共存 | PASS | 2026-11-06 / Stage 2 | Wi-Fi 信道6、Zigbee信道15并行运行；保持 Web 可用并完成一次 D1 入网；长时压力仍未测试 |
| HW-201 | 3 | QBKG25LM 加入网络 | PASS | 2026-11-06 / Stage 2 | 180秒窗口内收到 Device announce，短地址 0x56FC；端点发现尚未执行 |
| HW-202 | 3 | Active Endpoint 记录 | PASS | 2026-11-06 / Stage 3 | 0x56FC 实测端点 1、2、3、242；242 描述符失败不影响三路 On/Off |
| HW-203 | 3 | 输入/输出 Cluster 记录 | PASS | 2026-11-06 / Stage 3 | ep1/2/3 均实测输入 Cluster 0x0006；未使用 Zigbee2MQTT 上层属性名代替底层端点 |
| HW-204 | 3 | 左路实际端点映射 | PASS | 2026-11-06 / Stage 3 | 实体左键 OFF 后只读回读 ep1=OFF，恢复后 ep1=ON |
| HW-205 | 3 | 中路实际端点映射 | PASS | 2026-11-06 / Stage 3 | 实体中键 OFF 后只读回读 ep2=OFF，恢复后 ep2=ON |
| HW-206 | 3 | 右路实际端点映射 | PASS | 2026-11-09 / Stage 4 | 近距离复测确认右路为 ep3，网页 OFF/ON 与实际灯具状态一致；现场名称为“会议室” |
| HW-301 | 4 | 三路独立 ON/OFF | NOT TESTED | — | 映射确认后执行 |
| HW-302 | 4 | 实体按键状态反馈 | NOT TESTED | 2026-11-09 / Stage 4 | 实测 D1 未主动上报实体按键后的 OnOff 状态；已加入 15 秒只读轮询补偿，等待刷新页面复测后再判定 |
| HW-303 | 4 | 全部关闭 | NOT TESTED | — | 等待硬件 |
| HW-304 | 4 | NTP/断网定时安全策略 | NOT TESTED | — | 等待硬件 |
| HW-305 | 4 | Wi-Fi/Zigbee 掉线恢复 | NOT TESTED | — | 等待硬件 |
| HW-306 | 4 | 72 小时稳定性与内存趋势 | NOT TESTED | — | 等待硬件 |
| HW-307 | 4 | 多条每日关灯计划独立触发且不重复 | NOT TESTED | — | 测试 17:00、19:30、20:30、21:30、23:59 |
| HW-308 | 4 | 左/中/右各最多8条及同分钟独立 OFF | NOT TESTED | — | 不得调用全关代替指定端点 |
| HW-309 | 4 | 有效时间下离线运行、有限补执行与加班保护 | NOT TESTED | — | 不跨日、不无限重试 |
| HW-401 | 5 | GPIO18 TX / GPIO19 RX 电气与空闲状态 | NOT TESTED | — | 先确认模块电压和引脚 |
| HW-402 | 5 | LG 明确 OFF 三次稳定学习 | NOT TESTED | — | 不得以 Toggle 代替 |
| HW-403 | 5 | 两组红外码 NVS 持久化及拆除 RX | NOT TESTED | — | 等待硬件 |
| HW-404 | 5 | 单台 LG 学习码重放实际关机 | NOT TESTED | — | “已发送”不等于实际关闭 |
| HW-405 | 5 | 一个发射模块覆盖两台空调 | NOT TESTED | — | 先一台再两台 |
| HW-406 | 5 | 空调独立/复用灯光时间定时 | NOT TESTED | — | NTP、断网、同分钟防重复 |
| HW-407 | 5 | Wi-Fi + Zigbee + RMT 共存 | NOT TESTED | — | 记录 Heap、掉线与超时 |
| HW-408 | 5 | 1W 模块供电压降及 ESP32 复位 | NOT TESTED | — | 不假设板载5V可提供350mA |
| HW-409 | 5 | 红外功能72小时稳定性 | NOT TESTED | — | 等待硬件 |
| HW-410 | 5 | 空调1/空调2/共享组各最多8条独立定时 | NOT TESTED | — | 只用已确认 OFF 码，状态不声称已关闭 |
| HW-411 | 4/5 | Phase A/存储/备份/传感器协议 C 测试固件 18 项在目标板执行 | NOT TESTED | — | C6 测试镜像已 BUILD PASS；QEMU 18 项 SOFTWARE/SIMULATED PASS，尚未烧录 C6 |
| HW-412 | 4 | 多 Zigbee 设备/端点状态隔离与短地址复用 | NOT TESTED | — | 旧 OFF 不得串设备或确认新命令 |
| HW-413 | 4 | 缓存过期、主动回读超时及恢复后新反馈 | NOT TESTED | — | 页面应为 UNKNOWN/FAILED，收到新反馈后再确认 |
| HW-414 | 5 | IR 入队、RMT 完成、失败和结果超时状态链 | NOT TESTED | — | SENT 仍不代表空调已关闭 |
| HW-415 | 1 | Wi-Fi/Web/RTC 的 Free/Minimum Heap、Largest Block 与任务 HWM | NOT TESTED | — | 冷启动后记录，附固件 SHA-256 |
| HW-416 | 2 | Wi-Fi/Zigbee 共存、建网及 Permit Join 内存峰值 | NOT TESTED | — | 对照关键内存事件和 RF 日志 |
| HW-417 | 3 | Zigbee announce、端点/Cluster 发现内存峰值 | NOT TESTED | — | 保存发现完成/失败前后数据 |
| HW-418 | 4 | 三路控制、状态回读和调度并发内存/任务栈 | NOT TESTED | — | 包含回读超时和多个同分钟任务 |
| HW-419 | 5 | RMT 学习/发射及三个 I2C 设备并发资源峰值 | NOT TESTED | — | 传感器软件已集成；同时运行 Wi-Fi、Zigbee、Web 的实机峰值待测 |
| HW-420 | Cloud | RainMaker TLS 握手、MQTT 首连及重连内存峰值 | NOT TESTED | — | SDK/固件已集成，仅静态 size 已测；必须采集实机事件峰值 |
| HW-421 | 1～5 | 各 Stage 72 小时/7 天 Heap、Largest Block、最低 HWM 趋势 | NOT TESTED | — | 各自独立记录，不用编译结果代替 |
| HW-422 | 6 | 双 OTA 升级、回滚及 NVS/Zigbee/IR 数据迁移 | NOT TESTED | — | 当前仅有候选分区规划，尚未启用 |
| HW-423 | 1～5 | 主 NVS/runtime NVS 空间不足且不自动擦除 | NOT TESTED | — | 页面 stats、串口错误、配置保留均需实测 |
| HW-424 | 4/5 | 单项执行状态写入时断电和重启恢复 | NOT TESTED | — | QEMU 故障注入 PASS；真实 Flash 断电测试待执行 |
| HW-425 | 4/5 | 56 条任务、失败重试和 7 天日志/NVS 增长 | NOT TESTED | — | 验证无持久调试日志、无无限重试、记录 NVS entries |
| HW-426 | 1～5 | 用户确认恢复出厂设置及部分擦除失败处理 | NOT TESTED | — | 仅使用测试设备；确认文本必须精确匹配 |
| HW-427 | 5 | CORE 构建无扩展初始化/任务/专用缓冲 | NOT TESTED | — | 软件构建图已隔离，目标板待验证 |
| HW-428 | 5 | CORE_SENSOR 缺失 BH1750/SHT31 时 Core 正常启动 | NOT TESTED | — | 缺失只能记录诊断 |
| HW-429 | 5/Cloud | CORE_CLOUD/FULL 云不可用时本地控制与调度正常 | NOT TESTED | — | 传输路径已实现；断云隔离和重连须实机验证 |
| HW-430 | 5 | FULL→CORE 保留 Wi-Fi/Zigbee/IR/RTC/56 条任务 | NOT TESTED | — | 先按 ROLLBACK.md 完整备份 |
| HW-431 | 5 | 移除 BH1750/SHT31 后 DS3231 共享 I²C 正常 | NOT TESTED | — | 记录总线和 OSF 日志 |
| HW-432 | 5 | 8 MiB 备份、变体回退及恢复演练 | NOT TESTED | — | 禁止 erase-flash，校验所有 SHA-256 |
| HW-433 | 1～5 | 管理员认证后的配置导出、上传限流、48 KiB 限制与两阶段确认 | NOT TESTED | — | 浏览器和目标板 Web 实测待执行；仅在可信局域网使用 |
| HW-434 | 1～5 | 配置导入写入中断电后的事务回滚 | NOT TESTED | — | 主配置、56 条任务和两组 IR 码都必须恢复到导入前状态 |
| HW-435 | 3～5 | 新开发板导入备份后重新配对、重新发现端点并验证两组 LG OFF 码 | NOT TESTED | — | 禁止把旧 Zigbee 映射或配对信息当作有效网络数据恢复 |
| HW-436 | Sensor | BH1750 实际 Lux、五场景标定与整体夜间异常 | NOT TESTED | — | 单传感器不得断言具体灯路 |
| HW-437 | Sensor | SHT31 温湿度、CRC 错误、趋势与拔插恢复 | NOT TESTED | — | 不得把温度趋势虚报为空调状态 |
| HW-438 | Sensor | BH1750/SHT31 独立启停、重启恢复与旧数据失效 | NOT TESTED | — | 记录 Heap、任务 HWM 和 NVS 写入计数 |
| HW-439 | Sensor | DS3231+BH1750+SHT31 GPIO6/7 共享总线 | NOT TESTED | — | 检查上拉电压、地址、OSF 与并发恢复 |
| HW-440 | Variants | CORE 页面显示扩展未包含且按钮禁用 | NOT TESTED | — | Core 控制必须继续正常 |
| HW-441 | Backup | 禁用传感器时导出、FULL→CORE 导入跳过、回到 FULL 后设置仍保留 | NOT TESTED | — | 软件事务与兼容测试 PASS；真实 NVS/断电待测 |
| HW-442 | 4 | Coordinator 重启后 D1 无人工干预自动恢复通信 | FAIL | 2026-11-16 / 0.11.1-stage4 | PAN 0xDED3、信道15恢复成功；8/20/60/180秒四次有限探测均执行，但未收到 Device Announce/Device Update/OnOff 反馈，ZCL 发送最终失败。禁止标记为自动恢复成功 |
| HW-443 | 4 | 升级前完整 8 MiB Flash 备份 | PASS | 2026-11-16 | 已读取并校验 8,388,608 B；SHA-256 `172fa5d50f8aae0b71215c79e3e022a571f221fa5291acfc0974da42fdbec049`，备份只保存在本机忽略目录 |
| HW-444 | 4 | Zigbee SDK 1.x→2.0.5 NVRAM 非破坏迁移 | PASS | 2026-11-16 / 0.12.0-stage4 | 新 `zigbee_nvs` 初始化和迁移完成；原 `zb_storage` 保留，Coordinator 恢复 PAN `0xDED3`、信道15；未执行全片擦除 |
| HW-445 | 4 | Zigbee 2.0.5 升级后 Wi-Fi/NVS 保留 | PASS | 2026-11-16 / 0.12.0-stage4 | 固件启动并恢复原 STA 配置；串口确认应用版本和分区表正常 |
| HW-446 | 4 | Zigbee 2.0.5 升级后 D1 无人工干预自动恢复通信 | FAIL | 2026-11-16 / 0.12.0-stage4 | D1 曾短暂显示连接并返回三路状态，随后刷新又显示无响应，三路控制没有反馈；说明 2.0.5 已能恢复通信但尚不稳定，不能标记 PASS |
| HW-447 | 4 | COM10 长时间串口监视稳定性 | FAIL | 2026-11-16 | Windows CP210x 设备出现错误 121/端口短暂消失；属于主机 USB/驱动链路问题，不据此判定 ESP32 固件崩溃 |
| HW-448 | 4 | 长子设备超时、IEEE 地址解析及 20 dBm 发射功率候选修复 | PASS | 2026-11-16 / 0.12.1-stage4 | 非擦除烧录后恢复 PAN `0xDED3`/信道15；D1 无需长按重新配对即自动恢复；ep1/2/3 reporting 均成功，随后三路回读均为 ON、`ESP_OK`，RSSI 分别约 -54/-55/-55 dBm |
| HW-449 | 4 | D1 15分钟及隔夜持续在线、自动回读与控制稳定性 | NOT TESTED | — | 本轮只证明一次重启后的自动恢复；不能据此提前声称长期稳定，需保持供电并在后续复测网页回读、实体按键上报和定时关闭 |
| HW-450 | 4 | D1 静默后 Web/定时明确 ON/OFF 最终送达 | NOT TESTED | — | `0.12.1-stage4` 已加入 0/6/12/24/48 秒的 5 次有界重发；仅收到命令开始后的新鲜端点反馈才确认成功。新固件启动时 D1 自动 Announce，但首轮 reporting/回读返回 ERROR，尚未执行不会影响办公的真实继电器动作测试 |

状态变更时必须附串口日志文件名、测试条件和可复现步骤；FAIL 不得删除，应追加修复版本和复测结果。

## 2026-11-06 Stage 1 首次实机记录

- 测试板：ESP32-C6-DevKitC-1-N8，USB 串口 COM8；J5 出厂未导通，焊接跳线后板载电源和串口恢复。实机所刷 `build-stage1/esp32c6_light_gateway.bin` SHA-256：`761b1fe7713e37b2ce0abb566f4a7254148a8a4417b511c546c694d9a6e316a7`。
- 已确认：应用镜像可写入并校验、Wi-Fi 配置可跨复位保存、DHCP 获得 `192.168.1.197`、网页可通过 IPv4 访问、NTP 校时成功。DS3231 尚未连接时返回 `ESP_ERR_NOT_FOUND`，不阻塞系统。
- 发现并修复：备份恢复大对象占用 `app_main` 栈；旧版每次 HTTP Basic 请求执行 120,000 次 PBKDF2 导致 `httpd` 触发任务看门狗；NTP 回调在 critical section 内调用本地时间函数导致锁异常。修复版已完成 Wi-Fi/NTP/空闲运行复测，尚待管理员首次登录迁移复测。
- mDNS：串口显示 `http://lightgw.local` 已发布，但当前客户端无法解析，保留 FAIL；使用当前 DHCP IPv4 不受影响。
- 当前 Stage 1 通过不代表 Zigbee、DS3231、红外、传感器或 RainMaker 硬件通过；这些项目继续保持 `NOT TESTED`。

## 2026-11-06 Stage 2 首次实机记录

- 测试板及网络：ESP32-C6-DevKitC-1-N8，COM8；Wi-Fi `MiFi-0300` 信道6，Zigbee Coordinator 信道15。实机所刷 `build-stage2/esp32c6_light_gateway.bin` SHA-256：`0afe772426ffc16f82ada103d5645b2deae8dd60394e4c367bb74b76ec29bc24`。
- 首次缺陷：`esp_zb_start(false)` 发出 `ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP` 后，应用未启动 BDB initialization，导致 Web 显示 Zigbee 未就绪且无建网日志。增加该信号处理后，实机记录 `Initialize Zigbee stack`、`Restored Zigbee network PAN=0xdd9f channel=15`。
- Permit Join：页面请求180秒后串口记录 `Permit join status=180`，超时自动恢复 `status=0`。
- QBKG25LM：按官方方式进入蓝灯快速闪烁组网状态后，Coordinator 收到 `Device announce short=0x56fc`。D1 未显示预期的蓝灯慢闪两次，但设备公告是本次入网的协议栈证据。
- 资源观测：Zigbee恢复时 free heap约218.9 KiB、minimum约216.7 KiB、largest block约196 KiB；Permit Join及设备公告期间 minimum约191.2 KiB，系统和 Web 保持运行。任务 HWM仍未采集，因此不能据此声称长期内存余量充足。
- 当前只证明 Stage 2 加入网络；端点、Cluster、型号读取、三路物理映射和控制仍为 `NOT TESTED`，必须由 Stage 3/4继续验证。

## 2026-11-06 Stage 3 端点诊断实机记录

- 实机所刷 `build-stage3/esp32c6_light_gateway.bin` SHA-256：`643cbffd69f92fa7a7220afc85b491d50139a8b4b5a94c71f835432ce46ad7a2`；刷写未覆盖 NVS/`zb_storage`，PAN `0xDD9F`、信道15及设备短地址 `0x56FC` 均保留。
- 型号读取确认 `QBKG25LM`。Active Endpoint 实测为 `1、2、3、242`；端点1/2/3均提供 HA profile `0x0104`、On/Off server Cluster `0x0006`，端点242的 Simple Descriptor 失败不影响三路诊断。
- 增加管理员认证的只读端点刷新。串口明确记录 `no control command`；接口只发送 Read Attributes，不发送 ON/OFF。基线实测 `ep1=ON、ep2=ON、ep3=ON`。
- 左路：实体关闭后回读 `ep1=OFF、ep2=ON、ep3=ON`，再次按键恢复后回读 `ep1=ON`，确认左路=`ep1`。
- 中路：实体关闭后回读 `ep2=OFF`，再次按键恢复后回读 `ep2=ON`，确认中路=`ep2`。
- 右路：根据唯一剩余端点候选为 `ep3`，但右路测试期间属性回读未返回，故保持 `NOT TESTED`，不能用排除法冒充硬件确认。
- Zigbee RSSI 约 `-75` 至 `-82 dBm`，出现部分属性响应丢失/超时。正式 Stage 4 控制前应改善网关位置或距离，并再次直接验证右路 OFF/ON。
- 实测发现并修复重复 Device Announce 引发的并发发现：同一短地址只允许一组发现，失败或空结果不得覆盖有效端点；网络恢复后自动重发现已保存但未确认映射的设备。
- 办公时间实体测试必须每一路按两次恢复原状态；首次按键与恢复按键之间只执行只读刷新。测试前确认不会影响在场人员，长时间测试安排在下班后。

## 2026-11-06 Stage 4 首次启动记录

- 实机所刷 `build-stage4/esp32c6_light_gateway.bin` SHA-256：`f21a4d59c8287210cc748b46a1abb61df958dff9ecd9397732b42378956374bd`；未执行 `erase-flash`，保留 NVS 与 Zigbee 分区。
- 启动确认应用版本 `0.11.0-stage4`、8 MiB Flash、Wi-Fi 配置及 DHCP 地址 `192.168.1.197` 均正常恢复；NTP 再次同步成功。
- Coordinator 恢复 PAN `0xDD9F`、信道15，并读取到已保存的未确认设备 `0x56FC`。启动时的自动端点重发现因设备未应答而超时，不覆盖此前保存的有效端点诊断数据。
- 本次只验证 Stage 4 安全升级与启动恢复，没有保存三路映射，也没有发送 ON/OFF；`HW-301`～`HW-309` 继续保持 `NOT TESTED`。

## 2026-11-16 Stage 4 D1 重启恢复复测

- 测试板：乐鑫 ESP32-C6-DevKitC-1-N8，USB 串口 COM10，STA MAC `ac:eb:e6:d3:a9:a0`；固件 `0.11.1-stage4`，应用镜像 SHA-256 `8f651e9337f0dc116cb857a36f703981627b14aac9ea4a2b5e067254aa195084`。刷写只更新应用镜像，未擦除 NVS 或 Zigbee 存储。
- Coordinator 恢复结果：PASS。串口确认恢复 PAN `0xDED3`、信道15，并启用 Trust Center rejoin 接受策略。
- 新恢复逻辑：在启动后 8、20、60、180 秒对已保存设备执行最多四次状态探测；收到真实 OnOff 属性反馈后立即停止。该策略不会无限发送，不会把旧缓存状态当成本次成功。
- 中间一次启动曾收到 `Device update short=0xc1e1 status=0 tc_action=0 parent=0x0000` 和 `Device announce short=0xc1e1`，随后型号读取成功，证明当 D1 主动发起 secure rejoin 时 Coordinator 可以接受并更新短地址。
- 最终干净重启复测：FAIL。持续监视超过4分钟，四次探测均执行，但没有收到 D1 的 Device Update、Device Announce 或 OnOff 状态反馈，发送最终返回 `ESP_FAIL`。因此“Coordinator 网络恢复”和“D1 自动恢复通信”必须分别判定，后者尚未解决。
- 本轮首次复测时依赖为 `esp-zigbee-lib 1.6.8` / `esp-zboss-lib 1.6.4`。官方 2.0.4 发布说明包含 Coordinator 重启后 PanCoordinator 位丢失修复，因此随后在独立迁移分支升级并保留本固件和完整 Flash 备份作为回退。

## 2026-11-16 Zigbee SDK 2.0.5 迁移与刷机记录

- 当前实机固件：`0.12.0-stage4`，应用镜像 `build-zb2-stage4/esp32c6_light_gateway.bin`，SHA-256 `fb3bbb82bf66a67db0d6429e953d7f4780d4be8d8610b6573619fa2c487fc3eb`。
- 升级前完成 8 MiB 全 Flash 备份并验证 SHA-256；备份文件位于被 Git 忽略的本机目录，不上传仓库。
- 新分区表在原分区末尾追加 `zigbee_nvs`，没有移动或覆盖原 `nvs`、factory app、`zb_storage`、coredump 和 `ir_data`。迁移失败时只禁用 Zigbee，Core 的 Wi-Fi/Web/手动诊断继续启动；代码不会自动擦除旧数据。

## 2026-11-16 D1 间歇通信复测与候选修复

- 用户实测 D1 偶尔能够自行恢复并显示三路状态，但数分钟后再次无响应，网页控制也未得到设备反馈；HW-446 因此记录为 `FAIL`，而非把一次短暂响应当作稳定恢复。
- 候选修复将 Coordinator 的终端设备老化时间设为 16384 分钟；控制、手动刷新、后台轮询和有限恢复探测均先用 IEEE 地址解析当前短地址，避免继续向陈旧短地址发命令；配置发射功率为 20 dBm。
- 网页把“已连接/未连接”改成“最近有响应/等待设备响应”，并显示最近响应距今秒数。该文案只描述可观测证据，不把短时间静默直接判定为离线；过期状态仍保持未知，不能作为本次关灯成功证据。
- Stage 4 最终版本 `0.12.1-stage4`，应用镜像 SHA-256：`15bb22b5b2b5bbc8ed164a2e4ea9fd37a7c100b4877392b3b48718313c0d3a07`。应用、Bootloader、分区表均写入 100% 且 Hash 校验通过；未擦除用户 NVS、旧 Zigbee 源分区或 Zigbee 2.x NVRAM。
- 本次启动实测 D1 自动 Announce、ep1/2/3 发现及 reporting 配置成功，HW-448 改为 `PASS`。15分钟、隔夜及定时任务稳定性另列 HW-449，继续为 `NOT TESTED`。
- 最终 UI 固件重启后，有限恢复探测分别读取 ep1、ep2、ep3，三路均返回新鲜 ON 状态，RSSI 约 -54 至 -55 dBm；因此页面刷新后应点亮三个绿色“开”按钮。此证据只确认本次回读，不替代 HW-449 的长期测试。
- 操作页按钮新增新鲜状态指示：确认开启时“开”按钮亮绿色，确认关闭时“关”按钮亮红色；未知、超时或过期状态不高亮。该显示逻辑已通过 JavaScript 解析和源码契约测试，视觉与实体状态仍需用户在网页复核。
- 串口已确认 `zigbee_nvs` 准备完成、Coordinator 恢复 PAN `0xDED3` 和信道15、Wi-Fi STA 配置保留。以上仅证明栈数据迁移和网络恢复，不证明 D1 已恢复应用通信。
- 当前使用 2.0.5 官方 1.x 兼容层完成第一轮实机迁移，编译会产生弃用告警；后续仍需逐项改为 2.x 原生 API。此技术债不影响回退，但不得把“能编译/能建网”描述为完整原生 2.x 迁移完成。
- COM10 的 CP210x 主机端口在监视期间再次短暂失去响应，且测试电脑不在设备当前局域网，因而 D1 自动重连项目保持 `NOT TESTED`。必须看到新的 OnOff 回读或实际控制反馈后才能改为 PASS。

## 2026-11-16 D1 静默设备控制送达改造

- 应用镜像 SHA-256：`d6caa70e0c6c38c1ebb1fa28ea379c4b04b1a7af85a53bedfd0a4f12a41269a6`；已非擦除写入 COM10，Wi-Fi、Zigbee 网络和用户 NVS 均保留。
- 固件对同一明确 ON/OFF 指令最多在 0、6、12、24、48 秒发送 5 次，并在每次发送后安排状态回读。收到本次命令之后的新鲜匹配状态会取消后续重发；用户发出更新命令时，旧命令的延迟重发会由 generation 检查取消，防止加班人员重新开灯后又被旧 OFF 关闭。
- 启动日志确认 Coordinator 恢复 PAN `0xDED3`/信道15，D1 `0xc1e1` 自动 Announce 并重新发现 ep1/2/3；该轮 ZCL reporting/回读状态为 ERROR，故实际控制送达仍记为 HW-450 `NOT TESTED`，不得用“已上线”代替“可控制”。
