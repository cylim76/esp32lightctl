from pathlib import Path


root = Path(__file__).resolve().parents[1]
manifest = (root / "main/idf_component.yml").read_text(encoding="utf-8")
defaults = (root / "sdkconfig.defaults").read_text(encoding="utf-8")
partitions = (root / "partitions.csv").read_text(encoding="utf-8")
partitions_stage5 = (root / "partitions_stage5.csv").read_text(encoding="utf-8")
config = (root / "main/app_config.h").read_text(encoding="utf-8")
migration = (root / "main/zigbee_nvram_migration.c").read_text(encoding="utf-8")
manager = (root / "main/zigbee_manager.c").read_text(encoding="utf-8")
main = (root / "main/main.c").read_text(encoding="utf-8")

# Pin the verified 2.x component and use only Espressif's official 1.x API
# compatibility layer while the native-API conversion is still in progress.
assert 'espressif/esp-zigbee-lib: "=2.0.5"' in manifest
assert "esp-zboss-lib" not in manifest
assert "CONFIG_ZB_SDK_1xx=y" in defaults

# Preserve the v1.x raw partition at its original location and add a separate
# NVS destination. This makes migration recoverable and permits rollback.
legacy = next(line for line in partitions.splitlines() if line.strip().startswith("zb_storage,"))
destination = next(line for line in partitions.splitlines() if line.strip().startswith("zigbee_nvs,"))
assert ",  data, fat," in legacy and "0x40000" in legacy
assert ",  data, nvs," in destination and "0x10000" in destination
stage5_destination = next(
    line for line in partitions_stage5.splitlines() if line.strip().startswith("zigbee_nvs,")
)
assert ",  data, nvs," in stage5_destination and "0x10000" in stage5_destination
assert '#define APP_ZIGBEE_STORAGE_PARTITION  "zigbee_nvs"' in config
assert '#define APP_ZIGBEE_LEGACY_PARTITION   "zb_storage"' in config

# Migration must be explicit, one-way, and non-destructive. A failure disables
# only Zigbee; it must never erase the old partition or global user settings.
assert "esp_zigbee_nvram_migrate(&config)" in migration
assert ".source_partition_name = APP_ZIGBEE_LEGACY_PARTITION" in migration
assert ".destination_partition_name = APP_ZIGBEE_STORAGE_PARTITION" in migration
assert "nvs_flash_erase" not in migration
assert "esp_partition_erase" not in migration
assert "source retained" in migration
prepare = main.index("zigbee_nvram_prepare()")
start = main.index("zigbee_manager_start()")
assert prepare < start
assert "Zigbee disabled to preserve NVRAM after migration error" in main

# The 2.x stack must actually use the migrated NVS partition.
assert ".storage_partition_name = APP_ZIGBEE_STORAGE_PARTITION" in manager
assert "esp_zb_platform_config" not in manager
assert "zboss_api.h" not in manager

print("ZIGBEE_MIGRATION_CONTRACT_TEST PASS")
