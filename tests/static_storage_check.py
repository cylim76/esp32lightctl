#!/usr/bin/env python3
"""Fail closed on regressions in destructive erase and persistent logging policy."""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
failures: list[str] = []


def require(path: str, needle: str) -> None:
    text = (ROOT / path).read_text(encoding="utf-8")
    if needle not in text:
        failures.append(f"{path}: missing required marker {needle!r}")


for source in (ROOT / "main").glob("*.c"):
    text = source.read_text(encoding="utf-8")
    if "nvs_flash_erase" in text and source.name != "storage_manager.c":
        failures.append(f"{source.relative_to(ROOT)}: destructive NVS erase outside storage manager")
    if source.name != "storage_manager.c" and re.search(r"esp_partition_(?:erase_range|write)\s*\(", text):
        failures.append(f"{source.relative_to(ROOT)}: raw destructive partition operation")

scheduler = (ROOT / "main/scheduler.c").read_text(encoding="utf-8")
for forbidden in ('nvs_set_blob(nvs, "exec_v1"', 'nvs_set_blob(nvs, "log_v1"'):
    if forbidden in scheduler:
        failures.append(f"main/scheduler.c: legacy whole-blob write remains: {forbidden}")
require("main/scheduler.c", 'nvs_open_from_partition(APP_RUNTIME_STORAGE_PARTITION')
require("main/scheduler.c", 'snprintf(key, 8, "ex%02u", index)')
require("main/scheduler.c", "runtime_present[APP_MAX_SCHEDULES]")
require("main/scheduler.c", "if (runtime_present[i]) continue")
require("main/storage_manager.c", 'FACTORY_RESET_CONFIRMATION')
require("main/storage_manager.c", 'nvs_get_stats')
require("main/web_server.c", 'ERASE_ALL_USER_DATA')
require("partitions.csv", "runtime_nvs")
require("partitions_stage5.csv", "runtime_nvs")

inventory = []
pattern = re.compile(
    r"\b(nvs_set_[a-z0-9_]+|nvs_commit|nvs_erase_key|nvs_flash_erase(?:_partition)?|"
    r"esp_partition_(?:write|erase_range)|fwrite|fopen)\s*\("
)
for source in sorted((ROOT / "main").glob("*.c")):
    for lineno, line in enumerate(source.read_text(encoding="utf-8").splitlines(), 1):
        for match in pattern.finditer(line):
            inventory.append(f"{source.relative_to(ROOT)}:{lineno}:{match.group(1)}")

print(f"STORAGE_STATIC inventory operations={len(inventory)}")
for item in inventory:
    print(f"  {item}")
if failures:
    for failure in failures:
        print(f"STORAGE_STATIC FAIL: {failure}", file=sys.stderr)
    raise SystemExit(1)
print("STORAGE_STATIC PASS")
