"""Короткий поток активности gateway для живого наблюдения.

Он намеренно не заменяет decision journal: тот остаётся единственным
источником списанной стоимости и полного обучающего снимка. Здесь лежат
только безопасные переходы запроса, чтобы монитор мог честно показать
отправку к провайдеру до готового ответа.
"""
import contextlib
import fcntl
import json
import sys
import threading
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterator


class Activity:
    """Атомарно дописывает ограниченный JSONL-поток активности."""

    def __init__(self, path: Path, *, journal_path: Path | None = None,
                 max_bytes: int = 10 * 1024 * 1024, rotations: int = 3) -> None:
        self.path = path
        self.lock_path = path.with_name(path.name + ".lock")
        self.max_bytes = max_bytes
        self.rotations = rotations
        if journal_path is not None:
            source = journal_path.resolve()
            activity_files = {path.resolve(), self.lock_path.resolve(),
                              *(self._rotated(index).resolve()
                                for index in range(1, rotations + 1))}
            # resolve ловит равные пути и symlink до появления файла; samefile
            # отдельно закрывает уже существующую hardlink-коллизию.
            same_file = (source.exists() and any(
                candidate.exists() and candidate.samefile(source)
                for candidate in activity_files))
            if source in activity_files or same_file:
                raise ValueError("activity path must not overlap decision journal")
        self._state_lock = threading.Lock()
        self._write_failures = 0

    def status(self) -> dict[str, int | str]:
        with self._state_lock:
            return {"path": str(self.path), "write_failures": self._write_failures,
                    "max_bytes": self.max_bytes, "rotations": self.rotations}

    @contextlib.contextmanager
    def lock(self, *, shared: bool) -> Iterator[None]:
        """Даёт читателю shared flock на согласованную границу ротации."""
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with self.lock_path.open("a", encoding="utf-8") as handle:
            flag = fcntl.LOCK_SH if shared else fcntl.LOCK_EX
            fcntl.flock(handle.fileno(), flag)
            try:
                yield
            finally:
                fcntl.flock(handle.fileno(), fcntl.LOCK_UN)

    def write(self, event: str, record: dict[str, Any]) -> bool:
        """Записывает один факт; сбой наблюдателя не меняет ответ gateway."""
        row = {
            "activity_schema_version": "1",
            "event_id": uuid.uuid4().hex,
            "ts": datetime.now(timezone.utc).isoformat(
                timespec="milliseconds").replace("+00:00", "Z"),
            "event": event,
            **record,
        }
        try:
            line = json.dumps(row, ensure_ascii=False, separators=(",", ":"),
                              allow_nan=False) + "\n"
            encoded = line.encode("utf-8")
            with self.lock(shared=False):
                self._rotate_if_needed(len(encoded))
                with self.path.open("ab") as handle:
                    handle.write(encoded)
                    handle.flush()
            return True
        except Exception as exc:
            with self._state_lock:
                self._write_failures += 1
            print(f"woj activity write failed: {type(exc).__name__}",
                  file=sys.stderr, flush=True)
            return False

    def _rotate_if_needed(self, incoming_bytes: int) -> None:
        if (not self.path.exists()
                or self.path.stat().st_size + incoming_bytes <= self.max_bytes):
            return
        self._rotated(self.rotations).unlink(missing_ok=True)
        for index in range(self.rotations - 1, 0, -1):
            older = self._rotated(index)
            if older.exists():
                older.replace(self._rotated(index + 1))
        self.path.replace(self._rotated(1))

    def _rotated(self, index: int) -> Path:
        return self.path.with_name(f"{self.path.name}.{index}")
