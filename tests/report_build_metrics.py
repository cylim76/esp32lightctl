"""Print reproducible firmware size and SHA-256 rows for completed builds."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
BUILDS = [
    *(f"build-quality-stage{stage}" for stage in range(1, 6)),
    *(f"build-quality-variant-{variant}" for variant in
      ("CORE", "CORE_SENSOR", "CORE_CLOUD", "FULL")),
]


def main() -> int:
    print("build | bin_bytes | flash_non_ram | diram | data | bss | sha256")
    for name in BUILDS:
        build = ROOT / name
        binary = build / "esp32c6_light_gateway.bin"
        map_file = build / "esp32c6_light_gateway.map"
        if not binary.is_file() or not map_file.is_file():
            print(f"MISSING: {name}", file=sys.stderr)
            return 2
        result = subprocess.run(
            [sys.executable, "-m", "esp_idf_size", "--format", "json", str(map_file)],
            check=True,
            capture_output=True,
            text=True,
        )
        metrics = json.loads(result.stdout)
        digest = hashlib.sha256(binary.read_bytes()).hexdigest()
        print(
            f"{name} | {binary.stat().st_size} | {metrics['used_flash_non_ram']} | "
            f"{metrics['used_diram']} | {metrics['diram_data']} | "
            f"{metrics['diram_bss']} | {digest}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
