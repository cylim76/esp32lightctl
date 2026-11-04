# ESP RainMaker Classic design

Status: **IMPLEMENTED / BUILD PASS / SOFTWARE CONTRACT TEST PASS / HARDWARE AND CLOUD NOT TESTED**.

## Compatibility decision

The cloud variants pin ESP-IDF 5.5.1, `espressif/esp_rainmaker` 1.17.0 and
`espressif/rmaker_common` 1.9.0. ESP RainMaker 1.17.0 declares ESP-IDF 5.1 or
newer and lists ESP32-C6 support. CORE and CORE_SENSOR do not resolve or link
the RainMaker component.

The component is an adapter above the Core Device Control Service:

1. RainMaker callbacks submit a target and desired state to `device_control`.
2. Core queues Zigbee or IR work and keeps its own command result.
3. The adapter reports a light state only after fresh per-device/per-endpoint
   Zigbee feedback. Expired state and read timeouts are rejected.
4. AC devices expose an explicit `Send OFF` trigger. A completed RMT send is
   reported as `IR sent; air-conditioner physical state unknown`, never as a
   confirmed physical OFF state.

The node exposes three independent light switches, All Lights OFF, LG AC 1
OFF, LG AC 2 OFF, LG AC Group OFF and a read-only environment device. The
environment values are sent only while their local sensor data is valid.

## Offline and failure isolation

RainMaker uses its own work queue, TLS and MQTT connection. A claim, DNS, TLS,
MQTT or user-mapping failure updates extension diagnostics but does not stop
Wi-Fi Web control, Zigbee, IR, RTC or the local scheduler. No local HTTP
endpoint is exposed to the public Internet.

Cloud values delivered with `ESP_RMAKER_REQ_SRC_INIT` are ignored, so a stale
cloud cache cannot overwrite current local switch state after a restart. Local
physical changes are synchronized only from a fresh Zigbee cache entry.

## Runtime disable limitation

The 1.17.0 implementation of `esp_rmaker_stop()` sets
`ESP_RMAKER_STATE_STOP_REQUESTED`, but does not expose a completion event that
allows this project to prove that node teardown and reinitialization are safe.
For that reason, disabling RainMaker saves the runtime setting and asks for a
controlled reboot. The next boot omits RainMaker initialization while Core
continues normally. Binding/claim data is not erased. This is intentionally
safer than force-freeing SDK resources still referenced by its tasks.

## Security boundaries

The administrator-only local mapping endpoint accepts a user ID and one-time
secret, passes them to `esp_rmaker_start_user_node_mapping()`, clears its local
buffers and never writes them to the project configuration or backup. The Web
page is HTTP Basic without TLS, so this flow is for a trusted isolated LAN only.
The phone app or CLI must independently submit the matching secret to the
RainMaker cloud. Production deployment should move to the SDK's recommended
challenge-response mapping workflow after it is validated with this project's
custom Wi-Fi provisioning path.

Official references:

- <https://github.com/espressif/esp-rainmaker/blob/master/README.md>
- <https://github.com/espressif/esp-rainmaker/blob/master/components/esp_rainmaker/idf_component.yml>
- <https://github.com/espressif/esp-rainmaker/blob/master/components/esp_rainmaker/include/esp_rmaker_core.h>
- <https://docs.rainmaker.espressif.com/docs/product_overview/concepts/user-node-mapping/>
