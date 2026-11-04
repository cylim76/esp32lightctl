from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def text(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


manager = text("main/extension_manager.c")
server = text("main/web_server.c")
page = text("main/web/index.html")
bh = text("extensions/bh1750/bh1750_extension.c")
sht = text("extensions/sht31/sht31_extension.c")

# Three independent, versioned runtime switches. They must not collapse the
# two sensors into one setting, and absent modules must be rejected by API.
for key in ("bh_enabled", "sht_enabled", "rm_enabled"):
    assert key in manager
assert "if (!module_included(module)) return ESP_ERR_NOT_SUPPORTED" in manager
assert "if (unchanged) return ESP_OK" in manager
assert "bh1750_extension_read_config(&current)" in manager
assert "sht31_extension_read_config(&current)" in manager

# Authenticated management and status routes exist in every variant so CORE
# can truthfully report that an extension is not compiled in.
for route in (
    "/api/extensions",
    "/api/extensions/toggle",
    "/api/extensions/bh1750/config",
    "/api/extensions/bh1750/calibrate",
    "/api/extensions/sht31/config",
):
    assert route in server
assert server.count("if (!backup_authorized(req)) return ESP_OK;") >= 7
assert "Module is not included in this firmware" in server

# Drivers perform actual measurement commands and retain only bounded RAM
# history. Probe-only implementations do not satisfy these checks.
assert "BH1750_COMMAND_CONTINUOUS_HIGH_RES" in bh
assert "i2c_master_receive" in bh
assert "BH1750_EXTENSION_HISTORY_COUNT" in bh
assert "SHT31_COMMAND_MEASURE_HIGH_REPEATABILITY" in sht
assert "i2c_master_transmit(s_device, command" in sht
assert "i2c_master_receive(s_device, frame" in sht
assert "sht31_protocol_decode" in sht
assert "SHT31_EXTENSION_HISTORY_COUNT" in sht

# User-facing semantics distinguish included/enabled/running/online and avoid
# claiming a single BH1750 identifies a particular light or an AC state.
for label in ("当前固件不包含", "用户已启用", "运行中", "最近错误"):
    assert label in page
assert "整体环境光照异常" in page
assert "不代表空调实际状态" in page
assert "编译成功不等于云端功能可用" in page

print("EXTENSION_RUNTIME_CONTRACT PASS")
