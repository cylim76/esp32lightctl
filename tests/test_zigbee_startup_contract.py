from pathlib import Path


source = (Path(__file__).resolve().parents[1] / "main/zigbee_manager.c").read_text(encoding="utf-8")
retry = (Path(__file__).resolve().parents[1] / "main/zigbee_control_retry.c").read_text(encoding="utf-8")
config = (Path(__file__).resolve().parents[1] / "main/app_config.h").read_text(encoding="utf-8")
header = (Path(__file__).resolve().parents[1] / "main/zigbee_manager.h").read_text(encoding="utf-8")
web = (Path(__file__).resolve().parents[1] / "main/web_server.c").read_text(encoding="utf-8")
page = (Path(__file__).resolve().parents[1] / "main/web/index.html").read_text(encoding="utf-8")
router = (Path(__file__).resolve().parents[1] / "router_firmware/main/router_main.c").read_text(encoding="utf-8")
schedule_section_start = page.index('<section id="scheduleCard"')
schedule_section = page[schedule_section_start:page.index("</section>", schedule_section_start)]

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
assert "queue_read(" in diag_body
assert "esp_zb_zcl_on_off_cmd_req" not in diag_body
assert "zigbee_manager_diagnostic_read" in header
assert '"/api/zigbee/diagnostic-read"' in web
assert "b.textContent='刷新'" in page
assert "b.className='channel-refresh'" in page
assert "只读刷新端点状态，不会开关灯" in page
assert "stage>=3&&isD1&&(d.onoff_server_endpoints||[]).length" in page
assert "const names=['出口','企划','会议室']" in page
assert "scheduleTargets=['出口照明','企划照明','会议室照明'" in page
assert ".switches{grid-template-columns:repeat(3,minmax(0,1fr))" in page
assert "on.textContent='开'" in page and "off.textContent='关'" in page
assert "knownOn=shown===1" in page
assert "knownOff=shown===0" in page
assert "turn-on${knownOn?' state-active':''}" in page
assert "turn-off${knownOff?' state-active':''}" in page
assert ".turn-on.state-active" in page and ".turn-off.state-active" in page
assert "on.onclick=()=>setSwitch(d.short,i,true)" in page
assert "off.onclick=()=>setSwitch(d.short,i,false)" in page
assert 'class="channel-state ${statusClass}"' in page or 'class="channel-state ${statusClass}" title=' in page
assert "x.display_valid?x.display_state:-1" in page
assert "x.read_pending?'刷新中':'未知'" in page
assert 'id="zigbeeDeviceDiagnostics"' in page
assert "`${names[channel]}${on?'开启':'关闭'}`" in page
assert 'id="scheduleCards"' in page
assert "function addScheduleCard(" in page
assert "MAX_SCHEDULE_CARDS=8" in page
assert 'class="entry-time" type="time"' in page
assert "class=\"entry-hour\"" not in page and "class=\"entry-minute\"" not in page
assert "统一失败重试与补执行" in page
assert "补执行有效期（分钟）" in page
assert "${name}</label>" in page
assert ">周${n}</label>" not in page
assert "定时关闭任务" in page
assert 'id="scheduleStatus"' not in page
assert "schedule-time-dot.disabled" in page
assert 'id="operationLux"' in page and 'id="operationClimate"' in page
assert 'id="zigbeePairActions"' in page
assert "开放 Zigbee 配对 180 秒" in page
assert ">全部关闭</button>" in page
assert "确认执行全部关闭？" in page
assert "全部关灯" not in page and "也不控制空调" not in page
assert "Aqara 智能开关 D1" in page
assert "ESP32-C6 Zigbee 路由器" in page
assert "ESP32-C6-Zigbee-Router" in config
assert "'E','S','P','3','2','-','C','6','-','Z','i','g','b','e','e','-','R','o','u','t','e','r'" in router
assert "ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID" in router
assert "ESP_ZB_HA_HOME_GATEWAY_DEVICE_ID" in router
assert '"router-heartbeat"' in source
assert "Zigbee Router heartbeat" in source
assert "device-identity" in page
assert "网络短地址" in page
assert "该设备尚未识别为 Aqara D1，不提供控制按钮" not in page
assert "路由器仅扩展 Zigbee 覆盖，不提供照明开关" not in page
assert '"device_kind"' in web and '"zigbee_router"' in web
assert '"display_valid"' in web and '"display_age_seconds"' in web
assert "APP_ZIGBEE_DISPLAY_HOLD_MS    150000" in config
assert "映射：${d.mapping_confirmed" not in page
assert "APP_ZIGBEE_STATE_POLL_MS" in source
assert 'xTaskCreate(zigbee_io_task, "zigbee_io"' in source
assert "APP_ZIGBEE_READ_STAGGER_MS" in source
assert "ZIGBEE_CONTROL_RETRY_ATTEMPTS" in source
assert "zigbee_control_retry_has_next" in source
assert "control_is_current(operation)" in source
assert "control_already_confirmed(operation)" in source
assert "6000U" in retry and "12000U" in retry and "24000U" in retry and "48000U" in retry
assert "APP_CONTROL_CONFIRM_TIMEOUT_MS 65000" in config
assert "ESP_ZB_ZCL_CMD_ON_OFF_TOGGLE_ID" not in source
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
assert "ZIGBEE_IO_CONFIG_REPORT" in mapping_body
assert "queue_all_states" in mapping_body
assert "esp_zb_zcl_write_attr_cmd_req" not in source
restored = source.index('ESP_LOGI(TAG, "Restored Zigbee network')
poll = source.index("poll_persisted_devices();", restored)
assert restored < poll

# A quiet QBKG25LM end device must not be aged out after the SDK default child
# timeout.  Every active I/O path also refreshes the current network address
# from the persisted IEEE address before issuing a request.
assert "ezb_nwk_set_ed_timeout(EZB_NWK_ED_TIMEOUT_16384MIN)" in source
assert "ezb_nwk_set_tx_power(APP_ZIGBEE_TX_POWER_DBM)" in source
assert "ezb_address_short_by_extended" in source
for reason in ("control", "manual-refresh", "background-poll", "recovery"):
    assert f'resolve_short_address(ieee, addr, "{reason}")' in source or \
           f'resolve_short_address(pending[i].ieee, pending[i].addr, "{reason}")' in source or \
           f'resolve_short_address(ieee, short_addr, "{reason}")' in source

# The Web UI reports evidence age instead of declaring a sleepy device
# disconnected solely because no packet arrived during a short display window.
assert "last_seen_age_seconds" in web
assert "d.last_seen_age_seconds>=0" in page
assert "最近有响应" not in page and "等待设备响应" not in page
assert '<section id="devicesCard" class="tab-panel"' in page

print("ZIGBEE_STARTUP_CONTRACT_TEST PASS")
