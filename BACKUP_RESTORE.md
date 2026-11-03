# 配置备份与恢复

## 目标与边界

Web 管理页可以下载一个带版本和 SHA-256 完整性校验的 JSON 文件，并在另一块 ESP32-C6-DevKitC-1-N8 上执行“验证 → 人工确认 → 提交”。该功能用于恢复办公室节能规则，不是 Flash/NVS 镜像工具。

备份格式 v1 包含：

- 左、中、右、全部照明以及 LG 空调 1/2/组的全部定时关闭任务，最多 56 条；
- 固定时区 `CST-8`（Asia/Shanghai，UTC+8）；
- Stage 5 的两组完整 IR OFF 码，包括原始时序结构、载波配置和内部 CRC；
- BH1750、SHT31、RainMaker 的独立非敏感配置 schema。目前这些实验组件还没有用户可调参数，因此 v1 导出空的 `non_sensitive_settings` 对象，不会伪造或写入无效设置。

以下数据明确不导出，也不会在导入时删除或覆盖：

- Wi-Fi SSID/密码和管理员密码；
- RainMaker 凭据、身份和云端密钥；
- Zigbee 网络密钥、网络状态、配对数据、设备表及端点映射；
- RTC 当前时间、运行状态、执行日志和 Coredump。

新板恢复后必须重新配对 Aqara D1、重新发现实际端点，并经过现场逐路确认后保存映射。旧映射不能证明新 Coordinator 上的设备已配对。

## 文件结构与完整性

顶层只有 `document` 和 `integrity`。摘要计算对象是 `document` 经 cJSON 紧凑序列化后的 UTF-8 字节：

```json
{
  "document": {
    "format_version": 1,
    "product": "esp32lightctl",
    "device_type": "ESP32-C6-DevKitC-1-N8",
    "firmware": "0.11.0-core",
    "source_variant": "CORE",
    "created_at_utc": "2026-11-03T12:00:00Z",
    "created_time_valid": true,
    "sensitive_data_included": false,
    "payload": {}
  },
  "integrity": {
    "algorithm": "SHA-256",
    "scope": "canonical-document-json",
    "digest": "...64 lowercase hex characters..."
  }
}
```

SHA-256 用于检测损坏和意外修改，不是数字签名；能修改文件的人也能重新计算摘要。因此导入仍必须使用管理员认证，并且只能在可信本地网络或安全通道内操作。

若设备时间可信，`created_at_utc` 是 UTC ISO-8601；时间无效时为 `UNAVAILABLE`，并设置 `created_time_valid=false`。这不会阻止备份，但用户应自行记录下载时间。

## Web 操作

### 导出

1. 登录管理页，打开“配置备份与恢复”。
2. 点击“导出配置 JSON”并确认当前网络可信。
3. 浏览器下载 `esp32lightctl-backup-v1-时间.json`。
4. 将文件保存在受控位置；虽然不含密码和网络密钥，定时与 IR 数据仍属于办公室配置。

### 导入

1. 在新板烧录兼容固件并完成新的 Wi-Fi/管理员首次配置。
2. 选择 JSON 文件，点击“验证导入文件”。此阶段不写配置。
3. 检查页面显示的定时数、IR 码数、跳过项及“需要重新配对”。
4. 点击“确认并恢复”，再次人工确认。
5. 恢复后重新配对 Aqara D1、发现端点、逐路验证并确认映射。
6. 对两组 LG OFF 码先做单台实机发送测试，再启用空调定时。

导入文件最大 48 KiB；连续上传至少间隔 10 秒；验证 token 只在 RAM 中保存 2 分钟，超时自动释放。验证候选和 JSON 解析缓冲都按需分配，不常驻 RAM。

## 验证与兼容规则

导入前一次性检查：

- JSON、产品名、开发板类型、格式版本、Core schema、ESP32-C6 target；
- `sensitive_data_included` 必须为 false；
- `SHA-256` 和 canonical scope；
- 时区只能是当前支持的 `CST-8`；
- 总任务数不超过 56，每个目标不超过 8，时间/星期/重试/补执行窗口全部在范围内；
- IR 每个槽位唯一，解码后长度必须等于当前 `ir_code_t`，内部 magic/version/长度/载波/占空比/总时长/CRC 全部有效；
- IR 语义必须为 `explicit_off_only`；
- 扩展配置版本必须是当前可识别版本，单个设置对象紧凑 JSON 不超过 1024 字节，并且不得声称包含凭据。

CORE、CORE_SENSOR、CORE_CLOUD、FULL 使用同一 Core schema 和 Stage 5 分区布局，可以交换 v1 文件。目标固件未编入的扩展配置会跳过并报告，Core 配置仍可恢复。Stage 1～4 也能读取定时配置；没有 IR 的固件会验证但跳过 IR 部分，之后应在 Stage 5/CORE 上重新导入以恢复红外码。

## 事务、断电与失败恢复

提交前，固件把将被修改的旧定时/时区/IR 数据写入 `runtime_nvs` 的 `backup_tx/rollback` 日志并提交。之后才按顺序写 Core NVS 和 `ir_data`。全部成功后才清除事务日志。

- 解析、范围或校验失败：没有任何配置写入；
- 某次 NVS 写入失败：立即尝试恢复导入前快照；
- 提交过程中断电：下次启动在 `settings_init()` 和 IR 初始化之前检测日志并恢复旧快照；
- 日志本身损坏：保留日志供诊断，不擦除任何分区，并明确记录 degraded/error；
- 成功重复导入同一文件：结果幂等，现有存储层会跳过未变化写入。

事务只修改定时、时区和 IR code key。它不会调用 `nvs_flash_erase()`、不会擦除整个 Flash、`zb_storage`、`zb_fct`、`sensor_nvs`、`cloud_nvs` 或 RainMaker 身份。

跨 NVS 分区不存在硬件原子提交，本项目通过持久化“旧值日志 + 启动前回滚”提供掉电一致性。仍需在开发板上执行拔电故障注入，才能把实物断电恢复标记为 PASS。

## HTTP 接口

所有接口要求已配置的管理员 HTTP Basic 认证；首次配网尚未建立管理员账户时返回 403。

- `GET /api/backup/export`
- `POST /api/backup/import/validate`：请求体为原始 JSON 文件，成功返回 token 和摘要。
- `POST /api/backup/import/commit`：`{"token":"...","confirm":"RESTORE_CONFIG"}`。

HTTP Basic 不提供加密。不要通过公共 Wi-Fi、不可信办公室网段或互联网端口映射使用这些接口。生产部署应放在受信 VLAN/隔离网络或由安全通道保护。

## 验证状态

- 主机契约/故障模型测试：已执行；
- ESP32-C6 Stage 1/Stage 5 编译：已执行；
- C SHA-256 测试：已编入 C6 测试固件；
- 浏览器到真实 ESP32 的导入/导出、真实 NVS 拔电、IR 码迁移、重新配对：**NOT TESTED**，等待硬件。
