"""SQLite state and durable asset storage."""

from __future__ import annotations

from contextlib import closing
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import sqlite3
from typing import Any


def _now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def _sync_directory(path: Path) -> None:
    try:
        fd = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    except OSError:
        return
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def _atomic_write(path: Path, body: bytes, mode: int = 0o600) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, mode)
    try:
        offset = 0
        while offset < len(body):
            offset += os.write(fd, body[offset:])
        os.fsync(fd)
    except BaseException:
        os.close(fd)
        temporary.unlink(missing_ok=True)
        raise
    os.close(fd)
    os.replace(temporary, path)
    _sync_directory(path.parent)


class Storage:
    """Persist sessions, assets, and generation jobs."""

    def __init__(self, database_path: Path, data_dir: Path, max_assets: int) -> None:
        self.database_path = database_path
        self.data_dir = data_dir
        self.sessions_dir = data_dir / "sessions"
        self.max_assets = max_assets
        data_dir.mkdir(parents=True, exist_ok=True)
        self.sessions_dir.mkdir(parents=True, exist_ok=True)
        self._initialize()

    def _connect(self) -> sqlite3.Connection:
        connection = sqlite3.connect(self.database_path, timeout=30)
        connection.row_factory = sqlite3.Row
        connection.execute("PRAGMA foreign_keys=ON")
        connection.execute("PRAGMA busy_timeout=30000")
        return connection

    def _initialize(self) -> None:
        with closing(self._connect()) as connection:
            connection.executescript(
                """
                PRAGMA journal_mode=WAL;
                PRAGMA synchronous=FULL;
                CREATE TABLE IF NOT EXISTS sessions (
                    session_id TEXT PRIMARY KEY,
                    device_id TEXT NOT NULL,
                    score_threshold INTEGER NOT NULL,
                    state TEXT NOT NULL CHECK(state IN
                        ('ACTIVE', 'QUEUED', 'PROCESSING', 'COMPLETED', 'FAILED')),
                    created_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL,
                    attempts INTEGER NOT NULL DEFAULT 0,
                    title TEXT,
                    storyboard_json TEXT,
                    video_path TEXT,
                    error TEXT
                );
                CREATE TABLE IF NOT EXISTS assets (
                    session_id TEXT NOT NULL REFERENCES sessions(session_id)
                        ON DELETE CASCADE,
                    sequence INTEGER NOT NULL,
                    score INTEGER NOT NULL,
                    raw_score REAL NOT NULL,
                    dhash TEXT NOT NULL,
                    sha256 TEXT NOT NULL,
                    path TEXT NOT NULL,
                    created_at TEXT NOT NULL,
                    PRIMARY KEY(session_id, sequence)
                );
                CREATE INDEX IF NOT EXISTS sessions_state_idx
                    ON sessions(state, updated_at);
                """
            )
            connection.commit()
        os.chmod(self.database_path, 0o600)

    def session_directory(self, session_id: str) -> Path:
        return self.sessions_dir / session_id

    def create_session(
        self, session_id: str, device_id: str, score_threshold: int
    ) -> tuple[dict[str, Any], bool]:
        now = _now()
        with closing(self._connect()) as connection:
            connection.execute("BEGIN IMMEDIATE")
            row = connection.execute(
                "SELECT * FROM sessions WHERE session_id=?", (session_id,)
            ).fetchone()
            if row is not None:
                if row["device_id"] != device_id or row["score_threshold"] != score_threshold:
                    connection.rollback()
                    raise ValueError("session id already exists with different metadata")
                connection.commit()
                return dict(row), False
            connection.execute(
                """INSERT INTO sessions
                   (session_id, device_id, score_threshold, state, created_at, updated_at)
                   VALUES (?, ?, ?, 'ACTIVE', ?, ?)""",
                (session_id, device_id, score_threshold, now, now),
            )
            connection.commit()
        directory = self.session_directory(session_id)
        (directory / "assets").mkdir(parents=True, exist_ok=True)
        _sync_directory(directory.parent)
        return self.get_session(session_id), True

    def store_asset(
        self,
        session_id: str,
        sequence: int,
        score: int,
        raw_score: float,
        dhash: str,
        body: bytes,
    ) -> tuple[dict[str, Any], bool]:
        digest = hashlib.sha256(body).hexdigest()
        path = self.session_directory(session_id) / "assets" / f"{sequence:04d}.bmp"
        with closing(self._connect()) as connection:
            connection.execute("BEGIN IMMEDIATE")
            session = connection.execute(
                "SELECT state FROM sessions WHERE session_id=?", (session_id,)
            ).fetchone()
            if session is None:
                connection.rollback()
                raise KeyError(session_id)
            existing = connection.execute(
                "SELECT * FROM assets WHERE session_id=? AND sequence=?",
                (session_id, sequence),
            ).fetchone()
            if existing is not None:
                same = (
                    existing["score"] == score
                    and abs(existing["raw_score"] - raw_score) < 0.000001
                    and existing["dhash"].lower() == dhash.lower()
                    and existing["sha256"] == digest
                )
                connection.commit()
                if not same:
                    raise ValueError("asset sequence already exists with different content")
                return dict(existing), False
            if session["state"] != "ACTIVE":
                connection.rollback()
                raise ValueError("session is no longer accepting assets")
            count = connection.execute(
                "SELECT COUNT(*) FROM assets WHERE session_id=?", (session_id,)
            ).fetchone()[0]
            if count >= self.max_assets:
                connection.rollback()
                raise OverflowError("session asset limit reached")

            _atomic_write(path, body)
            now = _now()
            connection.execute(
                """INSERT INTO assets
                   (session_id, sequence, score, raw_score, dhash, sha256, path,
                    created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)""",
                (session_id, sequence, score, raw_score, dhash.lower(), digest,
                 str(path), now),
            )
            connection.execute(
                "UPDATE sessions SET updated_at=? WHERE session_id=?",
                (now, session_id),
            )
            connection.commit()
            row = connection.execute(
                "SELECT * FROM assets WHERE session_id=? AND sequence=?",
                (session_id, sequence),
            ).fetchone()
            return dict(row), True

    def finish_session(self, session_id: str) -> dict[str, Any]:
        now = _now()
        with closing(self._connect()) as connection:
            connection.execute("BEGIN IMMEDIATE")
            row = connection.execute(
                "SELECT * FROM sessions WHERE session_id=?", (session_id,)
            ).fetchone()
            if row is None:
                connection.rollback()
                raise KeyError(session_id)
            if row["state"] == "ACTIVE":
                asset_count = connection.execute(
                    "SELECT COUNT(*) FROM assets WHERE session_id=?", (session_id,)
                ).fetchone()[0]
                if asset_count == 0:
                    connection.execute(
                        """UPDATE sessions SET state='FAILED', updated_at=?, error=?
                           WHERE session_id=?""",
                        (now, "cannot generate a Vlog without assets", session_id),
                    )
                else:
                    connection.execute(
                        """UPDATE sessions SET state='QUEUED', updated_at=?, error=NULL
                           WHERE session_id=?""",
                        (now, session_id),
                    )
            connection.commit()
        return self.get_session(session_id)

    def recover_jobs(self) -> int:
        now = _now()
        with closing(self._connect()) as connection:
            connection.execute("BEGIN IMMEDIATE")
            cursor = connection.execute(
                """UPDATE sessions SET state='QUEUED', updated_at=?
                   WHERE state='PROCESSING'""",
                (now,),
            )
            recovered = cursor.rowcount
            completed = connection.execute(
                """SELECT session_id, video_path FROM sessions
                   WHERE state='COMPLETED'"""
            ).fetchall()
            for row in completed:
                path = Path(row["video_path"]) if row["video_path"] else None
                if path is None or not path.is_file() or path.stat().st_size == 0:
                    connection.execute(
                        """UPDATE sessions SET state='QUEUED', video_path=NULL,
                           error='published video missing during recovery', updated_at=?
                           WHERE session_id=?""",
                        (now, row["session_id"]),
                    )
                    recovered += 1
            connection.commit()
            return recovered

    def claim_job(self) -> dict[str, Any] | None:
        with closing(self._connect()) as connection:
            connection.execute("BEGIN IMMEDIATE")
            row = connection.execute(
                """SELECT * FROM sessions WHERE state='QUEUED'
                   ORDER BY updated_at, session_id LIMIT 1"""
            ).fetchone()
            if row is None:
                connection.commit()
                return None
            now = _now()
            connection.execute(
                """UPDATE sessions SET state='PROCESSING', attempts=attempts+1,
                   updated_at=? WHERE session_id=? AND state='QUEUED'""",
                (now, row["session_id"]),
            )
            connection.commit()
        return self.get_session(row["session_id"])

    def complete_job(
        self, session_id: str, storyboard: dict[str, Any], video_path: Path
    ) -> None:
        now = _now()
        with closing(self._connect()) as connection:
            connection.execute(
                """UPDATE sessions SET state='COMPLETED', title=?, storyboard_json=?,
                   video_path=?, error=NULL, updated_at=? WHERE session_id=?""",
                (
                    storyboard.get("title", "AI Vlog"),
                    json.dumps(storyboard, ensure_ascii=False, separators=(",", ":")),
                    str(video_path),
                    now,
                    session_id,
                ),
            )
            connection.commit()

    def fail_job(self, session_id: str, error: str) -> None:
        with closing(self._connect()) as connection:
            connection.execute(
                """UPDATE sessions SET state='FAILED', error=?, updated_at=?
                   WHERE session_id=?""",
                (error[:1000], _now(), session_id),
            )
            connection.commit()

    def get_session(self, session_id: str) -> dict[str, Any]:
        with closing(self._connect()) as connection:
            row = connection.execute(
                """SELECT s.*,
                   (SELECT COUNT(*) FROM assets a WHERE a.session_id=s.session_id)
                   AS asset_count
                   FROM sessions s WHERE s.session_id=?""",
                (session_id,),
            ).fetchone()
            if row is None:
                raise KeyError(session_id)
            result = dict(row)
            if result.get("storyboard_json"):
                result["storyboard"] = json.loads(result["storyboard_json"])
            result.pop("storyboard_json", None)
            return result

    def list_completed_sessions(self) -> list[dict[str, Any]]:
        with closing(self._connect()) as connection:
            rows = connection.execute(
                """SELECT session_id, title, updated_at, video_path
                   FROM sessions WHERE state='COMPLETED'
                   ORDER BY updated_at DESC, session_id"""
            ).fetchall()
            return [dict(row) for row in rows]

    def list_assets(self, session_id: str) -> list[dict[str, Any]]:
        with closing(self._connect()) as connection:
            rows = connection.execute(
                "SELECT * FROM assets WHERE session_id=? ORDER BY sequence",
                (session_id,),
            ).fetchall()
            return [dict(row) for row in rows]
