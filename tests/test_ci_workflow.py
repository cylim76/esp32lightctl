from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
workflow = (ROOT / ".github/workflows/quality.yml").read_text(encoding="utf-8")

assert "matrix:\n        stage: [1, 2, 3, 4, 5]" in workflow
assert "matrix:\n        variant: [CORE, CORE_SENSOR, CORE_CLOUD, FULL]" in workflow

# Every shell block that invokes idf.py in the ESP-IDF container must first
# activate the image's toolchain in the same block. Environment changes made
# by a previous GitHub Actions step are not assumed to persist.
for block in re.findall(r"run: \|\n((?:          .*\n)+)", workflow):
    if "idf.py" in block:
        assert '. "$IDF_PATH/export.sh"' in block, block
        assert block.index('. "$IDF_PATH/export.sh"') < block.index("idf.py"), block

assert "PHASE_A_C_TEST SUMMARY PASS:" in workflow
assert re.search(r"SUMMARY PASS: \[0-9\]\+ tests", workflow)
assert "SUMMARY PASS: 15 tests" not in workflow
assert "SUMMARY PASS: 16 tests" not in workflow
assert 'grep -q "PHASE_A_C_TEST FAIL"' in workflow

for test in ("test_variant_rollback.py", "test_backup_contract.py",
             "test_backup_restore.py", "test_ci_workflow.py",
             "test_delivery_docs.py"):
    assert test in workflow

print("CI_WORKFLOW_CONTRACT PASS")
