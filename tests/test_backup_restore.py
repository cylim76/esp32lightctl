"""Executable host model for backup/restore safety and compatibility cases.

This does not claim target hardware validation. It exercises the documented
canonical JSON/checksum contract and the transaction state machine used by the
firmware, including simulated power loss.
"""
from __future__ import annotations

import copy
import hashlib
import json

MAX_BYTES = 48 * 1024
MAX_SCHEDULES = 56


def canonical(value: object) -> bytes:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode()


def make_backup() -> dict:
    document = {
        "format_version": 1,
        "product": "esp32lightctl",
        "device_type": "ESP32-C6-DevKitC-1-N8",
        "firmware": "test",
        "source_variant": "FULL",
        "created_at_utc": "2026-11-03T12:00:00Z",
        "created_time_valid": True,
        "sensitive_data_included": False,
        "payload": {
            "core_schema_version": 3,
            "target": "esp32c6",
            "timezone": "CST-8",
            "lighting": {
                "channels": ["left", "center", "right"],
                "zigbee_network_included": False,
                "device_mapping_included": False,
                "repair_and_rediscovery_required": True,
            },
            "schedules": [{
                "enabled": True, "target": 0, "hour": 18, "minute": 0,
                "days": 127, "retry_count": 3,
                "retry_offsets_minutes": [1, 5, 10],
                "catchup_window_minutes": 30,
            }],
            "lg_ir": {"available_in_source_firmware": False, "off_codes": [],
                      "command_semantics": "explicit_off_only"},
            "extensions": {
                name: {"included_in_source_firmware": False, "config_version": 1,
                       "non_sensitive_settings": {}, "credentials_included": False}
                for name in ("bh1750", "sht31", "rainmaker")
            },
            "excluded_sensitive_or_identity_data": [
                "wifi_password", "admin_password", "rainmaker_credentials",
                "zigbee_network_keys", "zigbee_pairing_data", "zigbee_device_mappings",
            ],
        },
    }
    return {"document": document, "integrity": {
        "algorithm": "SHA-256", "scope": "canonical-document-json",
        "digest": hashlib.sha256(canonical(document)).hexdigest(),
    }}


def validate(package: dict) -> None:
    raw = canonical(package)
    assert len(raw) <= MAX_BYTES
    document = package["document"]
    assert document["format_version"] == 1
    assert document["product"] == "esp32lightctl"
    assert document["device_type"] == "ESP32-C6-DevKitC-1-N8"
    assert document["sensitive_data_included"] is False
    assert package["integrity"]["algorithm"] == "SHA-256"
    assert package["integrity"]["scope"] == "canonical-document-json"
    assert hashlib.sha256(canonical(document)).hexdigest() == package["integrity"]["digest"]
    payload = document["payload"]
    assert payload["core_schema_version"] == 3 and payload["target"] == "esp32c6"
    assert payload["timezone"] == "CST-8"
    assert not payload["lighting"]["zigbee_network_included"]
    assert not payload["lighting"]["device_mapping_included"]
    schedules = payload["schedules"]
    assert len(schedules) <= MAX_SCHEDULES
    counts = [0] * 7
    for entry in schedules:
        target = entry["target"]
        counts[target] += 1
        assert 0 <= target < 7 and counts[target] <= 8
        assert 0 <= entry["hour"] <= 23 and 0 <= entry["minute"] <= 59
        assert 0 <= entry["days"] <= 127 and 0 <= entry["catchup_window_minutes"] <= 180
        assert len(entry["retry_offsets_minutes"]) == entry["retry_count"] <= 3
    assert payload["lg_ir"]["command_semantics"] == "explicit_off_only"


class TransactionalStore:
    def __init__(self, value: dict):
        self.value = copy.deepcopy(value)
        self.journal = None

    def begin(self, replacement: dict) -> None:
        self.journal = copy.deepcopy(self.value)
        self.value = copy.deepcopy(replacement)

    def commit(self) -> None:
        self.journal = None

    def boot_recover(self) -> None:
        if self.journal is not None:
            self.value = self.journal
            self.journal = None


def expect_rejected(mutator, label: str) -> None:
    package = make_backup()
    mutator(package)
    try:
        validate(package)
    except (AssertionError, KeyError, TypeError, IndexError):
        print(f"PASS {label}")
        return
    raise AssertionError(f"accepted invalid backup: {label}")


def main() -> None:
    package = make_backup()
    validate(package)
    print("PASS normal import")
    try:
        json.loads(canonical(package).decode() + " {}")
    except json.JSONDecodeError:
        print("PASS trailing JSON rejected")
    else:
        raise AssertionError("accepted trailing JSON document")
    expect_rejected(lambda p: p.pop("document"), "malformed JSON structure")
    expect_rejected(lambda p: p["document"].update(format_version=99), "incompatible version")
    expect_rejected(lambda p: p["document"]["payload"].update(timezone="UTC0"), "checksum failure")
    expect_rejected(lambda p: p["integrity"].update(digest="0" * 64), "explicit checksum failure")
    oversized = make_backup()
    oversized["document"]["payload"]["padding"] = "x" * MAX_BYTES
    oversized["integrity"]["digest"] = hashlib.sha256(canonical(oversized["document"])).hexdigest()
    expect_rejected(lambda p: p.update(oversized), "oversized file")

    old = {"schedules": ["old"], "ir": ["old-code"]}
    new = {"schedules": ["new"], "ir": ["new-code"]}
    store = TransactionalStore(old)
    store.begin(new)
    store.boot_recover()
    assert store.value == old and store.journal is None
    print("PASS power interruption rollback")
    store.begin(new)
    store.commit()
    assert store.value == new
    store.begin(new)
    store.commit()
    assert store.value == new
    print("PASS duplicate import is idempotent")
    assert package["document"]["payload"]["lighting"]["repair_and_rediscovery_required"]
    assert "zigbee_network_keys" in package["document"]["payload"]["excluded_sensitive_or_identity_data"]
    print("PASS new-board restore requires Zigbee repair and excludes secrets")
    print("BACKUP_RESTORE_HOST_TEST PASS")


if __name__ == "__main__":
    main()
