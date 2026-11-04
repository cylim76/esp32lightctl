# 扩展模块运行时管理

状态：Web/API、BH1750、SHT31 运行时启停以及 RainMaker Classic SDK 集成为 **IMPLEMENTED / SOFTWARE TEST PASS / BUILD PASS**。传感器实物、RainMaker Claiming、账户绑定、TLS/MQTT 和手机 App 行为仍为 **HARDWARE/CLOUD NOT TESTED**。

## 模块边界

Core 始终包含本地 Wi-Fi/Web、Zigbee、Aqara 控制、LG 红外、DS3231 和调度。`BH1750`、`SHT31`、`RainMaker` 是独立组件；未编译的组件不会链接驱动、创建采样任务或分配其历史缓冲。`extension_manager` 本身始终存在，因此 CORE 页面会如实显示“当前固件不包含”，而不是提供无效按钮。

运行时配置保存在独立分区：

| 模块 | 分区 | key | 默认值 |
| --- | --- | --- | --- |
| BH1750 | `sensor_nvs` | `module_mgr/bh_enabled` | 启用（仅当已编译） |
| SHT31 | `sensor_nvs` | `module_mgr/sht_enabled` | 启用（仅当已编译） |
| RainMaker | `cloud_nvs` | `module_mgr/rm_enabled` | 启用（仅当已编译） |

只有状态实际变化才提交 NVS。配置损坏只记录错误并采用安全默认值，不擦除 Core 或整个 Flash。

## Web API

所有写接口要求已配置的管理员账号和 HTTP Basic 认证：

- `GET /api/extensions`：三个模块状态、最近成功时间、最近错误、传感器数据和最多 20 个 RAM 样本。
- `POST /api/extensions/toggle`：`{"module":"bh1750|sht31|rainmaker","enabled":true|false}`。
- `POST /api/extensions/bh1750/config`：采样周期、夜间分钟范围、Lux 阈值、连续异常次数。
- `POST /api/extensions/bh1750/calibrate`：场景 `0..4`，只接受当前有效 Lux。
- `POST /api/extensions/sht31/config`：采样周期 `30..60` 秒。
- `POST /api/extensions/rainmaker/map`：启动用户—节点映射；只接受管理员认证请求，User ID/一次性 secret 不持久化、不返回页面。客户端仍须通过官方 App/CLI 提交匹配 secret。

禁用传感器会终止采样任务、移除 I²C device handle、清除有效标志和 RAM 历史；重新启用会重新探测并重新建立有效数据，旧样本不能冒充新结果。DS3231 使用共享总线管理器，传感器启停不会卸载总线。

RainMaker 1.17.0 的 `esp_rmaker_stop()` 只发出停止请求，没有暴露可证明任务和传输已完全销毁的完成事件。因此关闭云扩展时先保存禁用状态，再返回 `restart_required`；受控重启后不初始化 RainMaker。启用时可直接启动尚未初始化的云扩展。云端缓存的初始化值不会覆盖本地状态，灯状态仅由新鲜 Zigbee 反馈上报，红外结果不声明空调已关闭。

## 验收状态

- 三个独立开关、未编译模块保护、仅变化写 NVS：SOFTWARE TEST PASS。
- BH1750/SHT31 启停与重新探测路径：BUILD PASS；硬件 NOT TESTED。
- 反复启停无泄漏、共享 I²C 并发、断电后 NVS 恢复：HARDWARE NOT TESTED。
- RainMaker SDK 图、控制策略、账户映射入口和 CORE_CLOUD/FULL 构建：SOFTWARE TEST PASS / BUILD PASS。
- Claiming、账户绑定、TLS/MQTT、断线重连、手机 App 和受控重启后的停连：CLOUD/HARDWARE NOT TESTED。
