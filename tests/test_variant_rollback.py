from copy import deepcopy
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


class FlashModel:
    """Models partition preservation; this is not a hardware Flash test."""

    def __init__(self) -> None:
        self.partitions = {
            "nvs": {"wifi": "office", "admin": "pbkdf2", "schedules": list(range(56))},
            "zb_storage": {"network": "coordinator-network"},
            "ir_data": {"off_1": [9000, 4500], "off_2": [9100, 4400]},
            "runtime_nvs": {"completed_targets": 0x15},
            "sensor_nvs": {
                "bh1750_ext": {"enabled": True, "threshold": 8.5, "calibration": [0.2, 12, 11, 13, 35]},
                "sht31_ext": {"enabled": False, "sample_period": 45},
            },
            "cloud_nvs": {"rainmaker_ext": {"enabled": True}, "claim_identity": "opaque"},
        }

    def flash_variant(self, variant: str) -> dict:
        assert variant in {"CORE", "CORE_SENSOR", "CORE_CLOUD", "FULL"}
        # Normal variant flashing replaces only bootloader/partition-table/app.
        # Data partitions remain byte-for-byte unchanged in this model.
        return deepcopy(self.partitions)


def main() -> None:
    partition_table = read("partitions_stage5.csv")
    expected_order = [
        "nvs", "phy_init", "factory", "zb_storage", "zb_fct", "coredump",
        "ir_data", "runtime_nvs", "sensor_nvs", "cloud_nvs",
    ]
    positions = [partition_table.index(name) for name in expected_order]
    assert positions == sorted(positions)
    assert 'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_stage5.csv"' in read("sdkconfig.stage5")
    cmake = read("CMakeLists.txt")
    assert "sdkconfig.stage5" in cmake and "sdkconfig.variant.${_lightgw_variant_lower}" in cmake
    for variant in ("core", "core_sensor", "core_cloud", "full"):
        assert (ROOT / f"sdkconfig.variant.{variant}").is_file()
    print("VARIANT_ROLLBACK PASS: stable partition table for all variants")

    manager = read("main/extension_manager.c")
    skip = manager.index("if (!module_included(module))")
    init_partition = manager.index("storage_manager_init_optional_partition(module_partition(module))", skip)
    assert skip < init_partition
    assert "nvs_flash_erase" not in manager and "nvs_erase_all" not in manager
    print("VARIANT_ROLLBACK PASS: absent extensions skip without opening or erasing their partition")

    flash = FlashModel()
    before = deepcopy(flash.partitions)
    core_view = flash.flash_variant("CORE")
    assert core_view["nvs"] == before["nvs"]
    assert core_view["zb_storage"] == before["zb_storage"]
    assert core_view["ir_data"] == before["ir_data"]
    assert core_view["runtime_nvs"] == before["runtime_nvs"]
    assert core_view["sensor_nvs"] == before["sensor_nvs"]
    assert core_view["cloud_nvs"] == before["cloud_nvs"]
    print("SIMULATED PASS: FULL to CORE preserves core and extension data partitions")

    # Core remains usable and may update core data without interpreting extension data.
    flash.partitions["nvs"]["schedules"][0] = "updated-by-core"
    assert flash.partitions["sensor_nvs"] == before["sensor_nvs"]
    assert flash.partitions["cloud_nvs"] == before["cloud_nvs"]
    full_view = flash.flash_variant("FULL")
    assert full_view["nvs"]["schedules"][0] == "updated-by-core"
    assert full_view["sensor_nvs"] == before["sensor_nvs"]
    assert full_view["cloud_nvs"] == before["cloud_nvs"]
    print("SIMULATED PASS: CORE to FULL restores extension visibility without data loss")

    rollback = read("ROLLBACK.md")
    assert "erase-flash" in rollback and "backup-full-8mb.bin" in rollback
    assert "不得执行 `erase-flash`" in rollback
    print("VARIANT_ROLLBACK PASS: destructive erase is explicitly prohibited")


if __name__ == "__main__":
    main()
