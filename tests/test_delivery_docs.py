from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

REQUIRED_DOCS = (
    "MODULE_MANAGEMENT.md",
    "SENSOR_WIRING.md",
    "ENVIRONMENT_MONITORING.md",
    "RAINMAKER_DESIGN.md",
    "RAINMAKER_SETUP.md",
    "RAINMAKER_TEST_PLAN.md",
    "README.md",
    "BUILD.md",
    "QUALITY_TEST_REPORT.md",
    "HARDWARE_TEST_RECORD.md",
    "BACKUP_RESTORE.md",
    "FIRMWARE_VARIANTS.md",
    "ROLLBACK.md",
    "OMISSION_AUDIT.md",
)

for filename in REQUIRED_DOCS:
    assert (ROOT / filename).is_file(), f"missing delivery document: {filename}"

readme = (ROOT / "README.md").read_text(encoding="utf-8")
for filename in REQUIRED_DOCS:
    if filename != "README.md":
        assert filename in readme, f"README does not link delivery document: {filename}"

STATUS_CONTRACTS = {
    "MODULE_MANAGEMENT.md": (
        "IMPLEMENTED", "SOFTWARE TEST PASS", "BUILD PASS", "NOT TESTED"),
    "ENVIRONMENT_MONITORING.md": (
        "SOFTWARE TEST PASS", "HARDWARE NOT TESTED"),
    "RAINMAKER_DESIGN.md": (
        "IMPLEMENTED", "BUILD PASS", "SOFTWARE", "NOT TESTED"),
    "RAINMAKER_TEST_PLAN.md": (
        "SOFTWARE TEST PASS", "NOT TESTED"),
    "OMISSION_AUDIT.md": (
        "IMPLEMENTED", "PARTIALLY IMPLEMENTED", "BUILD PASS",
        "SOFTWARE TEST PASS", "NOT TESTED"),
}
for filename, markers in STATUS_CONTRACTS.items():
    text = (ROOT / filename).read_text(encoding="utf-8")
    for marker in markers:
        assert marker in text, f"{filename} does not declare status marker: {marker}"

hardware = (ROOT / "HARDWARE_TEST_RECORD.md").read_text(encoding="utf-8")
rows = [line for line in hardware.splitlines() if line.startswith("| HW-")]
assert rows, "hardware test record has no HW rows"
allowed = {"PASS", "FAIL", "NOT TESTED"}
statuses = []
for row in rows:
    fields = [field.strip() for field in row.split("|")]
    assert len(fields) >= 8, f"malformed hardware row: {row}"
    status = fields[4]
    assert status in allowed, f"invalid hardware status {status!r}: {row}"
    if status in {"PASS", "FAIL"}:
        assert fields[5] not in {"", "—", "-"}, (
            f"hardware {status} requires a dated firmware/test reference: {row}")
        assert fields[6] not in {"", "—", "-"}, (
            f"hardware {status} requires evidence or reproduction notes: {row}")
    statuses.append(status)
assert "NOT TESTED" in statuses, (
    "unverified modules must remain explicitly NOT TESTED")

variants = (ROOT / "FIRMWARE_VARIANTS.md").read_text(encoding="utf-8")
for variant in ("`CORE`", "`CORE_SENSOR`", "`CORE_CLOUD`", "`FULL`"):
    assert variant in variants, f"missing documented firmware variant: {variant}"

rollback = (ROOT / "ROLLBACK.md").read_text(encoding="utf-8")
assert "core-stable-v1.0" in rollback
assert "硬件" in rollback and "Tag" in rollback

print(f"DELIVERY_DOCS PASS: {len(REQUIRED_DOCS)} documents, {len(rows)} hardware rows")
