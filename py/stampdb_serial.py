import serial
import time
from typing import Iterator, Tuple

class StampDBSerial:
    def __init__(self, port: str, baud: int = 115200, timeout: float = 1.0):
        self.ser = serial.Serial(port=port, baudrate=baud, timeout=timeout)
        time.sleep(0.2)

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def write(self, series: int, ts_ms: int, value: float):
        line = f"w {series} {ts_ms} {value}\n".encode()
        self.ser.write(line)

    def flush(self):
        self.ser.write(b"f\n")

    def snapshot(self):
        self.ser.write(b"s\n")

    def latest(self, series: int) -> Tuple[int, float]:
        self.ser.write(f"l {series}\n".encode())
        resp = self.ser.readline().decode().strip()
        if not resp.startswith("OK "):
            raise RuntimeError(f"Bad response: {resp}")
        _, ts_s, val_s = resp.split()
        return int(ts_s), float(val_s)

    def export(self, series: int, t0: int, t1: int) -> Iterator[Tuple[int,float]]:
        self.ser.write(f"e {series} {t0} {t1}\n".encode())
        # Expect lines: ts,value until 'END' line
        while True:
            line = self.ser.readline().decode().strip()
            if not line:
                break
            if line == "END":
                return
            if "," in line:
                ts_s, val_s = line.split(",", 1)
                yield int(ts_s), float(val_s)

__all__ = ["StampDBSerial"]

