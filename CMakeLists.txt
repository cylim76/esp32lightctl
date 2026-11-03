cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Only traverse the application and its declared dependency graph. Besides
# making builds deterministic, this avoids pulling unrelated IDF components
# (Bluetooth, MQTT, OpenThread) into a Wi-Fi + Zigbee-only firmware.
idf_build_set_property(MINIMAL_BUILD ON)
project(esp32c6_light_gateway)
