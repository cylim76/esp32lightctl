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
assert "b.textContent='刷新'" in page
assert "b.className='channel-refresh'" in page
assert "只读刷新端点状态，不会开关灯" in page
assert "stage>=3&&(d.onoff_server_endpoints||[]).length" in page
assert "const names=['出口','企划','会议室']" in page
assert "scheduleTargets=['出口照明','企划照明','会议室照明'" in page
assert ".switches{grid-template-columns:repeat(3,minmax(0,1fr))" in page
assert "b.textContent=`${n}：${state(d.states[i])}`" in page
assert "`${names[channel]}${on?'开启':'关闭'}`" in page
assert "APP_ZIGBEE_STATE_POLL_MS" in source
assert 'xTaskCreate(state_poll_task, "zigbee_poll"' in source
assert "static bool begin_discovery" in source
assert "static void end_discovery" in source
assert "duplicate announce ignored" in source
assert "previous valid endpoints preserved" in source
assert "partial discovery merged" in source
assert "static bool merge_endpoint" in source
finish_start = source.index("static void finish_discovery")
finish_end = source.index("static void request_next_simple_desc", finish_start)
finish_body = source[finish_start:finish_end]
assert "mapping_confirmed = false" not in finish_body
assert "memset(s_devices[idx].endpoints" not in finish_body
assert "merge_endpoint(device->onoff_endpoints" in finish_body
assert "rediscover persisted unmapped device" in source
mapping_start = source.index("esp_err_t zigbee_manager_confirm_mapping")
mapping_end = source.index("esp_err_t zigbee_manager_set_channel", mapping_start)
mapping_body = source[mapping_start:mapping_end]
assert "configure_onoff_reporting" in mapping_body
assert "esp_zb_zcl_write_attr_cmd_req" not in source
restored = source.index('ESP_LOGI(TAG, "Restored Zigbee network')
poll = source.index("poll_persisted_devices();", restored)
assert restored < poll

print("ZIGBEE_STARTUP_CONTRACT_TEST PASS")
