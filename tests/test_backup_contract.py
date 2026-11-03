from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = (root / "main/backup_restore.c").read_text(encoding="utf-8")
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
]
for marker in required_source:
    assert marker in source, marker
assert main.index("backup_restore_recover_pending") < main.index("settings_init()")
for route in ("/api/backup/export", "/api/backup/import/validate", "/api/backup/import/commit"):
    assert route in web, route
assert "backup_authorized" in web and "429 Too Many Requests" in web
assert "配置备份与恢复" in html and "没有 TLS" in html and "重新配对" in html
print("BACKUP_CONTRACT_TEST PASS")
