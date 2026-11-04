# 扩展模块运行时管理

状态：Web/API 与 BH1750、SHT31 运行时启停为 **IMPLEMENTED / SOFTWARE TEST PASS / BUILD PASS / HARDWARE NOT TESTED**。RainMaker 开关和状态边界已实现，但真实云传输仍为 **NOT IMPLEMENTED**。

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

禁用传感器会终止采样任务、移除 I²C device handle、清除有效标志和 RAM 历史；重新启用会重新探测并重新建立有效数据，旧样本不能冒充新结果。DS3231 使用共享总线管理器，传感器启停不会卸载总线。

RainMaker 若所选 SDK 无法安全销毁/重建，将返回 `restart_required` 并要求受控重启；不得强行释放仍被 SDK 任务使用的资源。当前 RainMaker 传输未实现，因此不会虚报运行或在线。

## 验收状态

- 三个独立开关、未编译模块保护、仅变化写 NVS：SOFTWARE TEST PASS。
- BH1750/SHT31 启停与重新探测路径：BUILD PASS；硬件 NOT TESTED。
- 反复启停无泄漏、共享 I²C 并发、断电后 NVS 恢复：HARDWARE NOT TESTED。
- RainMaker Claiming/TLS/MQTT/真正停连：NOT IMPLEMENTED。
