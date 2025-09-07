import ctypes as _ct
import os as _os
from typing import Iterator, Optional, Tuple

_LIB_NAMES = [
    _os.environ.get("STAMPDB_LIB"),
    "build/mk/libstampdb_shared.dylib",
    "build/mk/libstampdb_shared.so",
    "build/host-debug/libstampdb_shared.dylib",
    "build/host-debug/libstampdb_shared.so",
    "libstampdb_shared.dylib",
    "libstampdb_shared.so",
]

def _load_lib():
    for p in _LIB_NAMES:
        if p and _os.path.exists(p):
            try:
                return _ct.CDLL(p)
            except OSError:
                continue
    # last resort: try just 'stampdb_shared'
    try:
        return _ct.CDLL("stampdb_shared")
    except OSError as e:
        raise RuntimeError("Could not locate stampdb_shared library. Set STAMPDB_LIB or build with CMake.") from e

_lib = _load_lib()

STAMPDB_OK=0

class _Cfg(_ct.Structure):
    _fields_ = [
        ("workspace", _ct.c_void_p),
        ("workspace_bytes", _ct.c_uint32),
        ("read_batch_rows", _ct.c_uint32),
        ("commit_interval_ms", _ct.c_uint32),
    ]

class _It(_ct.Structure):
    _fields_ = [
        ("s", _ct.c_void_p),
        ("series", _ct.c_uint16),
        ("t0", _ct.c_uint32),
        ("t1", _ct.c_uint32),
        ("seg_idx", _ct.c_uint32),
        ("page_in_seg", _ct.c_uint32),
        ("row_idx_in_block", _ct.c_uint32),
        ("count_in_block", _ct.c_uint16),
        ("dt_bits", _ct.c_uint8),
        ("t0_block", _ct.c_uint32),
        ("bias", _ct.c_float),
        ("scale", _ct.c_float),
        ("deltas", _ct.c_uint32*74),
        ("qvals", _ct.c_int16*74),
        ("times", _ct.c_uint32*74),
        ("values", _ct.c_float*74),
    ]

_lib.stampdb_open.argtypes = [_ct.POINTER(_ct.c_void_p), _ct.POINTER(_Cfg)]
_lib.stampdb_open.restype = _ct.c_int
_lib.stampdb_close.argtypes = [_ct.c_void_p]
_lib.stampdb_write.argtypes = [_ct.c_void_p, _ct.c_uint16, _ct.c_uint32, _ct.c_float]
_lib.stampdb_write.restype = _ct.c_int
_lib.stampdb_flush.argtypes = [_ct.c_void_p]
_lib.stampdb_flush.restype = _ct.c_int
_lib.stampdb_query_begin.argtypes = [_ct.c_void_p, _ct.c_uint16, _ct.c_uint32, _ct.c_uint32, _ct.POINTER(_It)]
_lib.stampdb_query_begin.restype = _ct.c_int
_lib.stampdb_next.argtypes = [_ct.POINTER(_It), _ct.POINTER(_ct.c_uint32), _ct.POINTER(_ct.c_float)]
_lib.stampdb_next.restype = _ct.c_bool
_lib.stampdb_query_end.argtypes = [_ct.POINTER(_It)]
_lib.stampdb_query_latest.argtypes = [_ct.c_void_p, _ct.c_uint16, _ct.POINTER(_ct.c_uint32), _ct.POINTER(_ct.c_float)]
_lib.stampdb_query_latest.restype = _ct.c_int
class _Stats(_ct.Structure):
    _fields_ = [
        ("seg_seq_head", _ct.c_uint32),
        ("seg_seq_tail", _ct.c_uint32),
        ("blocks_written", _ct.c_uint32),
        ("crc_errors", _ct.c_uint32),
        ("gc_warn_events", _ct.c_uint32),
        ("gc_busy_events", _ct.c_uint32),
        ("recovery_truncations", _ct.c_uint32),
    ]
_lib.stampdb_info.argtypes = [_ct.c_void_p, _ct.POINTER(_Stats)]

class StampDB:
    def __init__(self, workspace_bytes: int = 1<<20, read_batch_rows: int = 512, commit_interval_ms: int = 0):
        self._ws = _ct.create_string_buffer(workspace_bytes)
        self._cfg = _Cfg(_ct.addressof(self._ws), workspace_bytes, read_batch_rows, commit_interval_ms)
        self._db = _ct.c_void_p()
        rc = _lib.stampdb_open(_ct.byref(self._db), _ct.byref(self._cfg))
        if rc != STAMPDB_OK:
            raise RuntimeError(f"stampdb_open failed rc={rc}")

    def close(self):
        if self._db:
            _lib.stampdb_close(self._db)
            self._db = _ct.c_void_p()

    def write(self, series: int, ts_ms: int, value: float):
        rc = _lib.stampdb_write(self._db, series, ts_ms, value)
        if rc != STAMPDB_OK:
            raise RuntimeError(f"stampdb_write rc={rc}")

    def flush(self):
        rc = _lib.stampdb_flush(self._db)
        if rc != STAMPDB_OK:
            raise RuntimeError(f"stampdb_flush rc={rc}")

    def query(self, series: int, t0_ms: int, t1_ms: int) -> Iterator[Tuple[int,float]]:
        it = _It()
        rc = _lib.stampdb_query_begin(self._db, series, t0_ms, t1_ms, _ct.byref(it))
        if rc != STAMPDB_OK:
            raise RuntimeError(f"stampdb_query_begin rc={rc}")
        try:
            ts = _ct.c_uint32()
            val = _ct.c_float()
            while _lib.stampdb_next(_ct.byref(it), _ct.byref(ts), _ct.byref(val)):
                yield int(ts.value), float(val.value)
        finally:
            _lib.stampdb_query_end(_ct.byref(it))

    def latest(self, series: int) -> Optional[Tuple[int,float]]:
        ts = _ct.c_uint32()
        val = _ct.c_float()
        rc = _lib.stampdb_query_latest(self._db, series, _ct.byref(ts), _ct.byref(val))
        if rc != STAMPDB_OK:
            return None
        return int(ts.value), float(val.value)

    def snapshot(self):
        rc = _lib.stampdb_snapshot_save(self._db)
        if rc != STAMPDB_OK:
            raise RuntimeError(f"snapshot rc={rc}")

    def info(self):
        st = _Stats()
        _lib.stampdb_info(self._db, _ct.byref(st))
        return {
            "seg_seq_head": st.seg_seq_head,
            "seg_seq_tail": st.seg_seq_tail,
            "blocks_written": st.blocks_written,
            "crc_errors": st.crc_errors,
            "gc_warn_events": st.gc_warn_events,
            "gc_busy_events": st.gc_busy_events,
            "recovery_truncations": st.recovery_truncations,
        }

__all__ = ["StampDB"]
