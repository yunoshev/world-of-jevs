"""Append-only decision journal.

This file is load-bearing, not debug output. Tests were deliberately dropped
from phase 1, so replaying this journal is the only way to reproduce a bug. In
phase 6 the same records become the training set that distils Jev into Laya —
which is why the full input is written, not a summary of it.
"""
import json
import os
import threading
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

_lock = threading.Lock()


class Journal:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.path.parent.mkdir(parents=True, exist_ok=True)

    def write(self, record: dict[str, Any]) -> None:
        record = {"ts": datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z"), **record}
        # allow_nan=False on purpose: json.dumps would otherwise emit a bare
        # NaN token, which is not valid JSON. One such line poisons the file
        # for jq and for every loader that will read it as a training set.
        line = json.dumps(record, ensure_ascii=False, separators=(",", ":"), allow_nan=False)
        with _lock:
            with self.path.open("a", encoding="utf-8") as fh:
                fh.write(line + "\n")
                fh.flush()
                os.fsync(fh.fileno())
