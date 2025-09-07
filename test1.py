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



db = StampDB()
for i in range(5): db.write(1, i*1000, 25.0 + 0.1*i)
db.flush()
print("latest:", db.latest(1))
db.close()