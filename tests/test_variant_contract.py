from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


checks = {
    "four CMake variants": all(v in read("CMakeLists.txt") for v in
                                ("CORE_SENSOR", "CORE_CLOUD", "FULL")),
    "sensor directories conditional": 'extensions/bh1750' in read("CMakeLists.txt")
                                      and 'extensions/sht31' in read("CMakeLists.txt"),
    "cloud directory conditional": 'extensions/rainmaker' in read("CMakeLists.txt"),
    "core control has no RainMaker dependency": "rainmaker" not in read("main/device_control.c").lower()
                                               and "rainmaker" not in read("main/device_control.h").lower(),
    "RTC uses shared I2C": "i2c_bus_manager_get" in read("main/rtc_manager.c")
                           and "i2c_new_master_bus" not in read("main/rtc_manager.c"),
    "extensions have independent namespaces": "bh1750_ext" in read("extensions/bh1750/include/bh1750_extension.h")
                                              and "sht31_ext" in read("extensions/sht31/include/sht31_extension.h")
                                              and "rainmaker_ext" in read("extensions/rainmaker/include/rainmaker_extension.h"),
    "variant partitions preserve core order": read("partitions_stage5.csv").index("runtime_nvs")
                                               < read("partitions_stage5.csv").index("sensor_nvs")
                                               < read("partitions_stage5.csv").index("cloud_nvs"),
    "all variant defaults exist": all((ROOT / f"sdkconfig.variant.{name}").is_file() for name in
                                       ("core", "core_sensor", "core_cloud", "full")),
    "CORE disables optional modules": "CONFIG_LIGHTGW_VARIANT_CORE=y" in read("sdkconfig.variant.core")
                                      and "CORE_SENSOR=y" not in read("sdkconfig.variant.core")
                                      and "CORE_CLOUD=y" not in read("sdkconfig.variant.core")
                                      and "VARIANT_FULL=y" not in read("sdkconfig.variant.core"),
    "component graph uses stable build property": "LIGHTGW_VARIANT_NAME" in read("CMakeLists.txt")
                                                   and "idf_build_get_property" in read("main/CMakeLists.txt"),
    "sensor components have separate Kconfig switches": "LIGHTGW_BH1750_EXTENSION" in read("main/Kconfig.projbuild")
                                                       and "LIGHTGW_SHT31_EXTENSION" in read("main/Kconfig.projbuild"),
    "extensions start only behind config": "#if LIGHTGW_HAS_BH1750_EXTENSION" in read("main/extension_manager.c")
                                           and "#if LIGHTGW_HAS_SHT31_EXTENSION" in read("main/extension_manager.c")
                                           and "#if LIGHTGW_HAS_RAINMAKER_EXTENSION" in read("main/extension_manager.c"),
}

failed = [name for name, ok in checks.items() if not ok]
for name, ok in checks.items():
    print(f"VARIANT_CONTRACT {'PASS' if ok else 'FAIL'}: {name}")
if failed:
    raise SystemExit(f"variant contract failures: {', '.join(failed)}")
