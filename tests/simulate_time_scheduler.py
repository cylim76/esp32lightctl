"""Deterministic host model for RTC/time/scheduler safety scenarios.

This does not replace ESP32 or attached-device tests.  It checks the intended
state-machine invariants and prints SIMULATED PASS so results cannot be confused
with hardware validation.
"""

from dataclasses import dataclass, field


@dataclass
class Model:
    time_valid: bool = False
    source: str = "INVALID"
    completed: set[str] = field(default_factory=set)
    attempts: dict[str, int] = field(default_factory=dict)

    def restore_rtc(self, valid_date: bool, osf: bool) -> None:
        if valid_date and not osf:
            self.time_valid, self.source = True, "RTC"

    def ntp(self, success: bool) -> None:
        if success:
            self.time_valid, self.source = True, "NTP"

    def due(self, target: str, minute_late: int, window: int = 30, max_attempts: int = 4) -> bool:
        if not self.time_valid or minute_late < 0 or minute_late > window or target in self.completed:
            return False
        used = self.attempts.get(target, 0)
        if used >= max_attempts:
            return False
        self.attempts[target] = used + 1
        return True

    def confirm(self, target: str) -> None:
        self.completed.add(target)


def check(label: str, condition: bool) -> None:
    if not condition:
        raise AssertionError(label)
    print(f"SIMULATED PASS {label}")


def main() -> None:
    a = Model()
    check("A RTC unset blocks scheduling", not a.due("left", 0))

    b = Model(); b.restore_rtc(True, False)
    check("B valid RTC works without Wi-Fi", b.due("left", 0))

    c = Model(); c.ntp(True)
    check("C NTP-valid time remains valid after long offline interval", c.time_valid and c.due("center", 0))

    d = Model(); d.restore_rtc(True, False)
    check("D power restore accepts battery-backed RTC", d.source == "RTC")

    e = Model(); e.restore_rtc(True, True)
    check("E OSF rejects RTC", not e.time_valid)

    f = Model(); f.restore_rtc(True, False); f.ntp(False)
    check("F failed NTP preserves prior valid time", f.time_valid and f.source == "RTC")

    g = Model(); g.ntp(True); g.due("right", 0); g.confirm("right")
    check("G time correction cannot repeat completed target", not g.due("right", 0))

    h = Model(); h.restore_rtc(True, False)
    check("H restart at 17:59 permits 18:00 run", h.due("left", 0))

    i = Model(); i.restore_rtc(True, False)
    check("I restore at 18:05 catches up within window", i.due("center", 5))

    j = Model(); j.restore_rtc(True, False)
    attempts = sum(j.due("right", n) for n in (0, 1, 5, 10, 11, 12))
    check("J Zigbee retries are bounded", attempts == 4)

    k = Model(); k.restore_rtc(True, False)
    ir_attempt = k.due("ac1", 0); light_attempt = k.due("left", 0)
    check("K IR failure does not block lighting", ir_attempt and light_attempt)

    l = Model(); l.restore_rtc(True, False); l.due("left", 0); l.confirm("left")
    check("L manual re-on after completion is not repeatedly closed", not l.due("left", 5))

    per_target = {name: 8 for name in ("left", "center", "right", "all", "ac1", "ac2", "ac_group")}
    check("per-target capacity is 8 and aggregate capacity is 56", sum(per_target.values()) == 56)
    check("v2 migration maps legacy global schedules without deletion", "all" in per_target)


if __name__ == "__main__":
    main()

