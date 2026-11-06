from pathlib import Path


source = (Path(__file__).resolve().parents[1] / "main/zigbee_manager.c").read_text(encoding="utf-8")
header = (Path(__file__).resolve().parents[1] / "main/zigbee_manager.h").read_text(encoding="utf-8")
web = (Path(__file__).resolve().parents[1] / "main/web_server.c").read_text(encoding="utf-8")
page = (Path(__file__).resolve().parents[1] / "main/web/index.html").read_text(encoding="utf-8")

skip_case = source.index("case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:")
initialization = source.index(
    "esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION)",
    skip_case,
)
first_start = source.index("case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:")

assert skip_case < initialization < first_start
assert 'ESP_LOGI(TAG, "Initialize Zigbee stack")' in source
assert 'diagnostics_set_error(TAG, "failed to start Zigbee initialization")' in source

# Stage 3 mapping diagnostics must remain read-only until physical mapping is confirmed.
diag_start = source.index("esp_err_t zigbee_manager_diagnostic_read")
diag_end = source.index("esp_err_t zigbee_manager_confirm_mapping", diag_start)
diag_body = source[diag_start:diag_end]
assert "send_read(" in diag_body
assert "esp_zb_zcl_on_off_cmd_req" not in diag_body
assert "zigbee_manager_diagnostic_read" in header
assert '"/api/zigbee/diagnostic-read"' in web
assert "只读刷新端点状态（不会开关灯）" in page
assert "static bool begin_discovery" in source
assert "static void end_discovery" in source
assert "duplicate announce ignored" in source
assert "previous valid endpoints preserved" in source
assert "if (ctx->onoff_count > 0)" in source
assert "rediscover persisted unmapped device" in source
restored = source.index('ESP_LOGI(TAG, "Restored Zigbee network')
poll = source.index("poll_persisted_devices();", restored)
assert restored < poll

print("ZIGBEE_STARTUP_CONTRACT_TEST PASS")
