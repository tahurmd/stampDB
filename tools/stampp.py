#!/usr/bin/env python3
"""
stampp.py — ultra-simple Python CLI for quick interactions (one-word commands).

Usage examples:
  python3 tools/stampp.py reset
  python3 tools/stampp.py hello
  python3 tools/stampp.py peek
  python3 tools/stampp.py dump > out.csv
"""
import os, sys, time
from typing import Iterator, Tuple

def reset() -> None:
    # Honor env overrides used by the C sim layer
    flash = os.environ.get("STAMPDB_FLASH_PATH") or "flash.bin"
    meta_dir = os.environ.get("STAMPDB_META_DIR") or "."
    metas = [os.path.join(meta_dir, n) for n in ("meta_snap_a.bin","meta_snap_b.bin","meta_head_hint.bin")]
    for p in [flash, *metas]:
        try:
            os.remove(p)
            print(f"removed {p}")
        except FileNotFoundError:
            pass

def _load(build_dir: str = None):
    # Lazy import to avoid package confusion
    from py.stampdb import StampDB
    if build_dir:
        return StampDB(build_dir=build_dir)
    return StampDB()

def hello() -> None:
    db = _load()
    for i in range(20):
        db.write(1, i*100, 25.0 + 0.1*i)
    db.flush()
    # print a small CSV sample
    print("ts_ms,value")
    n=0
    for ts, v in db.query(1, 0, 2000):
        print(f"{ts},{v:.9g}")
        n += 1
        if n>=10: break
    db.close()

def peek() -> None:
    db = _load()
    row = db.latest(1)
    if row: print(f"{row[0]},{row[1]:.9g}")
    db.close()

def dump() -> None:
    db = _load()
    print("ts_ms,value")
    for ts, v in db.query(1, 0, 0xFFFFFFFF):
        print(f"{ts},{v:.9g}")
    db.close()

def main(argv: list[str]) -> int:
    if not argv:
        print(__doc__.strip())
        return 1
    cmd = argv[0]
    if cmd == "reset":
        reset(); return 0
    if cmd == "hello":
        hello(); return 0
    if cmd == "peek":
        peek(); return 0
    if cmd == "dump":
        dump(); return 0
    print(__doc__.strip()); return 1

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

