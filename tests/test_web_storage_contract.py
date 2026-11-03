#!/usr/bin/env python3
"""Source-level API/UI contract checks that need no target hardware."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
server = (ROOT / "main/web_server.c").read_text(encoding="utf-8")
page = (ROOT / "main/web/index.html").read_text(encoding="utf-8")

checks = {
    "authenticated factory-reset handler": "factory_reset_post" in server and "if (!authorized(req))" in server,
    "exact destructive confirmation": 'strcmp(confirm->valuestring, "ERASE_ALL_USER_DATA")' in server,
    "NVS statistics endpoint": '"storage"' in server and '"free_entries"' in server,
    "write error counters": '"write_failures"' in server and '"corruption_events"' in server,
    "UI confirmation prompt": "ERASE_ALL_USER_DATA" in page,
    "UI NVS capacity fields": "NVS" in page and "free_entries" in page,
}

failed = [name for name, ok in checks.items() if not ok]
for name, ok in checks.items():
    print(f"WEB_STORAGE_CONTRACT {'PASS' if ok else 'FAIL'}: {name}")
if failed:
    raise SystemExit(1)
