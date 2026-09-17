"""Recoverable single-instance background generation worker."""

from __future__ import annotations

import fcntl
import logging
import os
from threading import Event, Thread

from .config import Settings
from .mify import build_storyboard
from .renderer import render_vlog
from .storage import Storage

_LOG = logging.getLogger(__name__)


class GenerationWorker:
    """Claim queued sessions and generate Vlogs serially."""

    def __init__(self, settings: Settings, storage: Storage) -> None:
        self.settings = settings
        self.storage = storage
        self._wake = Event()
        self._stop = Event()
        self._thread = Thread(target=self._run, name="aivlog-worker", daemon=True)
        self._lock_fd: int | None = None

    def start(self) -> None:
        lock_path = self.settings.data_dir / "worker.lock"
        self._lock_fd = os.open(lock_path, os.O_RDWR | os.O_CREAT, 0o600)
        try:
            fcntl.flock(self._lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            os.close(self._lock_fd)
            self._lock_fd = None
            raise RuntimeError(
                f"another AI Vlog worker owns {lock_path}"
            ) from error
        recovered = self.storage.recover_jobs()
        if recovered:
            _LOG.warning("recovered %d interrupted generation jobs", recovered)
        self._thread.start()

    def notify(self) -> None:
        self._wake.set()

    def stop(self) -> None:
        self._stop.set()
        self._wake.set()
        if self._thread.is_alive():
            timeout = (
                self.settings.mify_timeout_seconds
                + self.settings.render_timeout_seconds
                + 10
            )
            self._thread.join(timeout=timeout)
            if self._thread.is_alive():
                _LOG.error(
                    "generation worker did not stop within %d seconds; "
                    "retaining the single-instance lock",
                    timeout,
                )
                return
        if self._lock_fd is not None:
            fcntl.flock(self._lock_fd, fcntl.LOCK_UN)
            os.close(self._lock_fd)
            self._lock_fd = None

    def _run(self) -> None:
        while not self._stop.is_set():
            job = self.storage.claim_job()
            if job is None:
                self._wake.wait(self.settings.worker_poll_seconds)
                self._wake.clear()
                continue
            session_id = job["session_id"]
            try:
                assets = self.storage.list_assets(session_id)
                storyboard = build_storyboard(self.settings, assets)
                video_path = render_vlog(
                    self.settings, session_id, storyboard, assets
                )
                self.storage.complete_job(session_id, storyboard, video_path)
                _LOG.info("completed Vlog session %s", session_id)
            except Exception as error:
                message = f"{type(error).__name__}: {error}"
                self.storage.fail_job(session_id, message)
                _LOG.exception("Vlog session %s failed", session_id)
