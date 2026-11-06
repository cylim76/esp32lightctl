from pathlib import Path

source = (Path(__file__).resolve().parents[1] / "main/time_manager.c").read_text(encoding="utf-8")

# Newlib localtime/mktime acquire locks and must never run inside a critical section.
assert "s_status.next_ntp_sync = next_daily_ntp" not in source
assert source.count("time_t next_sync = next_daily_ntp(now);") >= 2
assert source.count("rtc.online ? rtc_manager_write_utc") >= 2
assert "previously_valid = s_status.valid;" in source
assert "NTP synchronization failed; system time remains invalid" in source
assert "NTP synchronization failed; previously valid local time remains valid" in source

print("TIME_MANAGER_CONTRACT_TEST PASS")
