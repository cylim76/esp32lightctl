from pathlib import Path


source = (Path(__file__).resolve().parents[1] / "main/zigbee_manager.c").read_text(encoding="utf-8")

skip_case = source.index("case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:")
initialization = source.index(
    "esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION)",
    skip_case,
)
first_start = source.index("case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:")

assert skip_case < initialization < first_start
assert 'ESP_LOGI(TAG, "Initialize Zigbee stack")' in source
assert 'diagnostics_set_error(TAG, "failed to start Zigbee initialization")' in source

print("ZIGBEE_STARTUP_CONTRACT_TEST PASS")
