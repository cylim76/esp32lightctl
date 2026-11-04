from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


extension = read("extensions/rainmaker/rainmaker_extension.c")
header = read("extensions/rainmaker/include/rainmaker_extension.h")
manager = read("main/extension_manager.c")
control = read("main/device_control.c")
server = read("main/web_server.c")
page = read("main/web/index.html")
manifest = read("extensions/rainmaker/idf_component.yml")

checks = {
    "SDK pinned for IDF 5.5.1": 'esp_rainmaker: "=1.17.0"' in manifest
                                   and 'version: "=5.5.1"' in manifest,
    "real node lifecycle": "esp_rmaker_node_init" in extension
                           and "esp_rmaker_start()" in extension,
    "self claim and MQTT events": "RMAKER_EVENT_CLAIM_FAILED" in extension
                                  and "RMAKER_MQTT_EVENT_CONNECTED" in extension,
    "user mapping": "esp_rmaker_start_user_node_mapping" in extension
                    and "/api/extensions/rainmaker/map" in server,
    "three independent lights": extension.count("Office Light ") == 3,
    "explicit AC OFF actions": all(name in extension for name in
                                    ("LG AC 1 OFF", "LG AC 2 OFF", "LG AC Group OFF"))
                               and "esp.param.explicit-off" in extension,
    "cloud uses core control service": "device_control_submit_target_state" in manager
                                       and "submit_target" in header,
    "light ON and OFF supported": "requested_on = on" in control
                                  and "on && target >= SCHEDULE_TARGET_AC_1" in control,
    "fresh Zigbee state only": "APP_ZIGBEE_STATE_MAX_AGE_MS" in manager
                               and "read_timed_out" in manager,
    "cached cloud init rejected": "ESP_RMAKER_REQ_SRC_INIT" in extension
                                  and "IGNORE_INIT" in extension,
    "AC state not fabricated": "air-conditioner physical state unknown" in extension,
    "environment reporting": all(token in extension for token in
                                  ("esp.param.lux", "ESP_RMAKER_DEF_TEMPERATURE_NAME", "Humidity")),
    "local Web warning": "HTTP 无 TLS" in page and "可信本地网络" in page,
    "controlled disable documented in code": "controlled reboot required" in extension,
}

failed = [name for name, ok in checks.items() if not ok]
for name, ok in checks.items():
    print(f"RAINMAKER_CONTRACT {'PASS' if ok else 'FAIL'}: {name}")
if failed:
    raise SystemExit("RainMaker contract failures: " + ", ".join(failed))
