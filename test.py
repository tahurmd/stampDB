#!/usr/bin/env python3
"""
Temp Sensor Demo (host) — write & read with StampDB via ctypes.

- Writes N temperature samples (series=1), 1000 ms apart.
- Flushes, closes, re-opens (simulated power cycle).
- Reads back the latest (ts_ms, value) and prints it.
- Ensures flash files live under --build-dir (we chdir there).

Usage:
  python3 examples/temp_sensor_demo.py --build-dir build/mk --n 20

Requires:
  - A shared library for StampDB in your build dir (libstampdb.so/.dylib/.dll)
  - Host build done via CMake (see KNOWLEDGEBASE.md “Run on PC”)
"""

import argparse
import ctypes as C
import os
import platform
import sys
from pathlib import Path

def die(msg):
    print(f"[x] {msg}", file=sys.stderr)
    sys.exit(1)

def find_lib(build_dir: Path):
    sysname = platform.system()
    pats = {
        "Darwin":  ["**/libstampdb.dylib", "libstampdb.dylib"],
        "Windows": ["**/stampdb.dll", "stampdb.dll"],
        "Linux":   ["**/libstampdb.so", "libstampdb.so"],
    }[sysname if sysname in ("Darwin","Windows") else "Linux"]
    for pat in pats:
        for p in build_dir.glob(pat):
            return p
    return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default="build/mk", help="Where to run & place flash.bin (we chdir here)")
    ap.add_argument("--series", type=int, default=1, help="Series id")
    ap.add_argument("--n", type=int, default=20, help="Number of samples to write")
    ap.add_argument("--step_ms", type=int, default=1000, help="Time between samples (ms)")
    ap.add_argument("--base_c", type=float, default=25.0, help="Base temperature (°C)")
    args = ap.parse_args()

    build = Path(args.build_dir).resolve()
    if not build.exists():
        die(f"Build dir not found: {build}")

    # Run everything inside build dir so flash.bin lands here
    os.chdir(build)
    print(f"[+] Working dir: {Path.cwd()}")
    print("[+] flash.bin, meta_snap_*.bin, meta_head_hint.bin will appear here.")

    # Clean old sim files for a fresh run (safe to ignore if absent)
    for fname in ("flash.bin","meta_snap_a.bin","meta_snap_b.bin","meta_head_hint.bin"):
        try: Path(fname).unlink()
        except FileNotFoundError: pass

    # Load shared lib
    libpath = find_lib(build)
    if not libpath:
        die("Could not find libstampdb shared library in build dir. Build the host target first.")
    print(f"[+] Using: {libpath}")
    lib = C.CDLL(str(libpath))

    # --- Minimal ABI types based on your header ---
    class StampDBCfg(C.Structure):
        _fields_ = [
            ("workspace", C.c_void_p),
            ("workspace_bytes", C.c_uint32),
            ("read_batch_rows", C.c_uint32),
            ("commit_interval_ms", C.c_uint32),
        ]

    # Function signatures
    lib.stampdb_open.argtypes  = [C.POINTER(C.c_void_p), C.POINTER(StampDBCfg)]
    lib.stampdb_open.restype   = C.c_int
    lib.stampdb_close.argtypes = [C.c_void_p]
    lib.stampdb_close.restype  = None
    lib.stampdb_write.argtypes = [C.c_void_p, C.c_uint16, C.c_uint32, C.c_float]
    lib.stampdb_write.restype  = C.c_int
    lib.stampdb_flush.argtypes = [C.c_void_p]
    lib.stampdb_flush.restype  = C.c_int
    lib.stampdb_query_latest.argtypes = [C.c_void_p, C.c_uint16, C.POINTER(C.c_uint32), C.POINTER(C.c_float)]
    lib.stampdb_query_latest.restype  = C.c_int

    RC = {0:"OK",1:"EINVAL",2:"EBUSY",3:"ENOSPACE",4:"ECRC",5:"EIO"}

    # Workspace buffers (deterministic RAM)
    ws1 = C.create_string_buffer(128*1024)
    ws2 = C.create_string_buffer(128*1024)

    # ---------- OPEN #1 ----------
    db = C.c_void_p()
    cfg = StampDBCfg(C.addressof(ws1), C.c_uint32(len(ws1)), C.c_uint32(512), C.c_uint32(0))
    rc = lib.stampdb_open(C.byref(db), C.byref(cfg))
    if rc != 0: die(f"stampdb_open #1 failed: {RC.get(rc, rc)}")

    # ---------- WRITE N SAMPLES ----------
    print(f"[+] Writing {args.n} samples (series={args.series}) …")
    for i in range(args.n):
        ts  = C.c_uint32(i * args.step_ms)
        val = C.c_float(args.base_c + 0.05*i)  # simple ramp (°C)
        rc = lib.stampdb_write(db, C.c_uint16(args.series), ts, val)
        if rc != 0: die(f"stampdb_write failed at i={i}: {RC.get(rc, rc)}")

    rc = lib.stampdb_flush(db)
    if rc != 0: die(f"stampdb_flush #1 failed: {RC.get(rc, rc)}")

    # Simulate power cycle
    lib.stampdb_close(db)

    # ---------- OPEN #2 (RECOVERY) ----------
    db2 = C.c_void_p()
    cfg2 = StampDBCfg(C.addressof(ws2), C.c_uint32(len(ws2)), C.c_uint32(512), C.c_uint32(0))
    rc = lib.stampdb_open(C.byref(db2), C.byref(cfg2))
    if rc != 0: die(f"stampdb_open #2 failed: {RC.get(rc, rc)}")

    # ---------- READ LATEST ----------
    out_ts = C.c_uint32(0)
    out_v  = C.c_float(0.0)
    rc = lib.stampdb_query_latest(db2, C.c_uint16(args.series), C.byref(out_ts), C.byref(out_v))
    if rc != 0: die(f"stampdb_query_latest failed: {RC.get(rc, rc)}")

    print(f"[+] Latest: ts_ms={out_ts.value}, temp_c={out_v.value:.3f}")
    expected_ts = (args.n - 1) * args.step_ms
    if out_ts.value != expected_ts:
        die(f"Unexpected ts: {out_ts.value} (expected {expected_ts})")

    lib.stampdb_close(db2)
    print("[✓] Demo OK. flash.bin path:", Path("flash.bin").resolve())

if __name__ == "__main__":
    main()
