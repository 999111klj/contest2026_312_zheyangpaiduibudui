"""Runtime configuration loaded from a protected environment file."""

from __future__ import annotations

from dataclasses import dataclass
from functools import lru_cache
import os
from pathlib import Path
import stat

_DEFAULT_ENV_FILE = Path.home() / ".config/aivlog-server/server.env"


def _load_env_file(path: Path) -> None:
    if not path.exists():
        return

    mode = stat.S_IMODE(path.stat().st_mode)
    if mode & 0o077:
        raise RuntimeError(f"configuration file must have mode 600: {path}")

    for number, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise RuntimeError(f"invalid configuration line {number}: {path}")
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip()
        if not key or not key.replace("_", "").isalnum():
            raise RuntimeError(f"invalid configuration key on line {number}")
        os.environ.setdefault(key, value)


def _positive_int(name: str, default: int) -> int:
    value = int(os.environ.get(name, str(default)))
    if value <= 0:
        raise RuntimeError(f"{name} must be positive")
    return value


def _bounded_float(name: str, default: float, minimum: float, maximum: float) -> float:
    value = float(os.environ.get(name, str(default)))
    if not minimum <= value <= maximum:
        raise RuntimeError(f"{name} must be between {minimum} and {maximum}")
    return value


@dataclass(frozen=True)
class Settings:
    """Validated server settings."""

    data_dir: Path
    database_path: Path
    device_token: str
    host: str
    port: int
    public_base_url: str
    mify_api_key: str
    mify_base_url: str
    mify_provider: str
    mify_model: str
    mify_timeout_seconds: int
    ffmpeg_binary: str
    render_timeout_seconds: int
    bgm_file: Path | None
    bgm_volume: float
    max_assets: int
    max_selected_assets: int
    worker_poll_seconds: int
    tls_certfile: Path | None
    tls_keyfile: Path | None

    @classmethod
    def load(cls) -> "Settings":
        env_path = Path(os.environ.get("AIVLOG_ENV_FILE", _DEFAULT_ENV_FILE)).expanduser()
        _load_env_file(env_path)

        data_dir = Path(
            os.environ.get(
                "AIVLOG_DATA_DIR", Path.home() / ".local/share/aivlog-server"
            )
        ).expanduser()
        device_token = os.environ.get("AIVLOG_DEVICE_TOKEN", "").strip()
        if len(device_token) < 32:
            raise RuntimeError("AIVLOG_DEVICE_TOKEN must contain at least 32 characters")

        max_assets = _positive_int("AIVLOG_MAX_ASSETS", 32)
        max_selected = _positive_int("AIVLOG_MAX_SELECTED_ASSETS", 12)
        if max_selected > max_assets:
            raise RuntimeError("AIVLOG_MAX_SELECTED_ASSETS cannot exceed AIVLOG_MAX_ASSETS")

        public_base_url = os.environ.get("AIVLOG_PUBLIC_BASE_URL", "").rstrip("/")
        bgm_value = os.environ.get("AIVLOG_BGM_FILE", "").strip()
        bgm_file = Path(bgm_value).expanduser().resolve() if bgm_value else None
        if bgm_file is not None and (
            not bgm_file.is_file() or not os.access(bgm_file, os.R_OK)
        ):
            raise RuntimeError(f"AIVLOG_BGM_FILE is not a readable file: {bgm_file}")
        return cls(
            data_dir=data_dir,
            database_path=data_dir / "aivlog.sqlite3",
            device_token=device_token,
            host=os.environ.get("AIVLOG_HOST", "127.0.0.1"),
            port=_positive_int("AIVLOG_PORT", 8000),
            public_base_url=public_base_url,
            mify_api_key=os.environ.get("MIFY_API_KEY", "").strip(),
            mify_base_url=os.environ.get(
                "MIFY_BASE_URL", "https://api.llm.mioffice.cn/v1"
            ).rstrip("/"),
            mify_provider=os.environ.get("MIFY_PROVIDER", "xiaomi"),
            mify_model=os.environ.get("MIFY_MODEL", "mimo-v2.5"),
            mify_timeout_seconds=_positive_int("MIFY_TIMEOUT_SECONDS", 120),
            ffmpeg_binary=os.environ.get("FFMPEG_BINARY", "ffmpeg"),
            render_timeout_seconds=_positive_int("AIVLOG_RENDER_TIMEOUT_SECONDS", 300),
            bgm_file=bgm_file,
            bgm_volume=_bounded_float("AIVLOG_BGM_VOLUME", 0.18, 0.0, 1.0),
            max_assets=max_assets,
            max_selected_assets=max_selected,
            worker_poll_seconds=_positive_int("AIVLOG_WORKER_POLL_SECONDS", 2),
            tls_certfile=(
                Path(os.environ["AIVLOG_TLS_CERTFILE"]).expanduser()
                if os.environ.get("AIVLOG_TLS_CERTFILE")
                else None
            ),
            tls_keyfile=(
                Path(os.environ["AIVLOG_TLS_KEYFILE"]).expanduser()
                if os.environ.get("AIVLOG_TLS_KEYFILE")
                else None
            ),
        )


@lru_cache(maxsize=1)
def get_settings() -> Settings:
    """Load and validate settings once for this process."""
    return Settings.load()
