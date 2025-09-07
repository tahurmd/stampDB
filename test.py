#!/usr/bin/env python3
"""
Quick host-side sanity test for StampDB using the Python ctypes wrapper.

Usage:
  python3 test.py [--fresh] [--series 1] [--rows 20]

Notes:
  - Writes a few rows into the simulator (flash.bin in repo root),
    flushes, prints latest, then exports a small range and prints the rows.
  - Set STAMPDB_LIB if the loader cannot find the shared library.
"""
import os
import sys
from typing import List, Tuple

try:
    # Preferred import when PYTHONPATH includes repo root
    from py.stampdb import StampDB  # may collide with external 'py' package
except Exception:
    # Fallback: import directly from the local ./py folder to avoid 'py' package clash
    import sys as _sys, os as _os
    _sys.path.insert(0, _os.path.join(_os.path.dirname(__file__), 'py'))
    from stampdb import StampDB


def reset_sim() -> None:
    for p in ["flash.bin", "meta_snap_a.bin", "meta_snap_b.bin", "meta_head_hint.bin"]:
        try:
            os.remove(p)
        except FileNotFoundError:
            pass


def main(argv: List[str]) -> int:
    # Parse simple flags
    fresh = "--fresh" in argv
    def get_arg(name: str, default: str) -> str:
        if name in argv:
            idx = argv.index(name)
            if idx + 1 < len(argv):
                return argv[idx + 1]
        return default

    series = int(get_arg("--series", "1"))
    rows = int(get_arg("--rows", "20"))

    if fresh:
        reset_sim()
        print("[info] reset simulator files (flash.bin, meta_*).")

    # Open DB with a 1 MiB workspace
    ws_bytes = 1 << 20
    db = StampDB(workspace_bytes=ws_bytes, read_batch_rows=512, commit_interval_ms=0)

    # Write rows: ts = i*100 ms, value = i*0.5
    for i in range(rows):
        db.write(series, i * 100, i * 0.5)
    db.flush()

    # Latest
    latest = db.latest(series)
    print(f"latest(series={series}):", latest)

    # Query a small range and print rows
    qrows: List[Tuple[int, float]] = list(db.query(series, 0, (rows - 1) * 100))
    print(f"query rows returned: {len(qrows)}")
    for r in qrows[:10]:
        print(f"{r[0]},{r[1]:.6g}")
    if len(qrows) > 10:
        print("...")

    # Snapshot + reopen sanity
    db.snapshot()
    stats = db.info()
    print("stats:", stats)
    db.close()

    # Re-open to prove recovery works quickly
    db2 = StampDB(workspace_bytes=ws_bytes, read_batch_rows=512, commit_interval_ms=0)
    latest2 = db2.latest(series)
    print(f"latest after reopen: {latest2}")
    db2.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
