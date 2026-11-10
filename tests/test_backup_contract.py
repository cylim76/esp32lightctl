from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = (root / "main/backup_restore.c").read_text(encoding="utf-8")
manager = (root / "main/extension_manager.c").read_text(encoding="utf-8")
bh1750 = (root / "extensions/bh1750/bh1750_extension.c").read_text(encoding="utf-8")
sht31 = (root / "extensions/sht31/sht31_extension.c").read_text(encoding="utf-8")
web = (root / "main/web_server.c").read_text(encoding="utf-8")
html = (root / "main/web/index.html").read_text(encoding="utf-8")
main = (root / "main/main.c").read_text(encoding="utf-8")

required_source = [
    'BACKUP_TRANSACTION_NAMESPACE "backup_tx"',
    "journal_save(previous)",
    "backup_restore_recover_pending",
    "backup_integrity_sha256_matches",
    "cJSON_ParseWithLengthOpts",
    "BACKUP_MAX_FILE_BYTES",
    '"zigbee_network_keys"',
    '"admin_password"',
    '"rainmaker_credentials"',
    '"explicit_off_only"',
    "extension_manager_get_backup_config",
    "extension_manager_apply_backup_config",
    '"calibration_lux"',
    '"sample_period_seconds"',
    "backup_transaction_v1_t",
]
for marker in required_source:
    assert marker in source, marker
recover = source[source.index("esp_err_t backup_restore_recover_pending"):
                 source.index("static cJSON *schedule_to_json")]
assert "backup_native_payload_t previous = {0};" not in recover
assert "backup_native_payload_t *previous = calloc(1, sizeof(*previous));" in recover
assert "if (!previous)" in recover
assert recover.count("free(previous);") >= 2
assert "bh1750_extension_read_config" in manager
assert "sht31_extension_read_config" in manager
assert "if (loaded) return ESP_OK;" in bh1750
assert "return load_config(config);" in bh1750
assert "if (loaded) return ESP_OK;" in sht31
assert "return load_config(config);" in sht31
assert main.index("backup_restore_recover_pending") < main.index("settings_init()")
for route in ("/api/backup/export", "/api/backup/import/validate", "/api/backup/import/commit"):
    assert route in web, route
assert "backup_authorized" in web and "429 Too Many Requests" in web
assert "配置备份与恢复" in html and "重新配对" in html
assert "连续上传至少间隔 10 秒" not in html
print("BACKUP_CONTRACT_TEST PASS")
