"""FastAPI application matching the camera_gallery upload contract."""

from __future__ import annotations

from contextlib import asynccontextmanager
import hmac
import math
from pathlib import Path
import re
import struct
from typing import Annotated, Any

from fastapi import Depends, FastAPI, Header, HTTPException, Request, status
from fastapi.responses import FileResponse, JSONResponse
from pydantic import BaseModel, ConfigDict, Field

from . import __version__
from .config import get_settings
from .storage import Storage
from .worker import GenerationWorker

_SESSION_PATTERN = re.compile(r"^[0-9a-f]{32}-[0-9a-f]{32}$")
_HEX_16_PATTERN = re.compile(r"^[0-9a-fA-F]{16}$")
_BMP_SIZE = 153666
_BMP_WIDTH = 320
_BMP_HEIGHT = 240


class SessionCreate(BaseModel):
    model_config = ConfigDict(extra="forbid")
    session_id: str = Field(min_length=65, max_length=65)
    device_id: str = Field(pattern=r"^[0-9a-fA-F]{32}$")
    score_threshold: int = Field(ge=0, le=100)


class SessionFinish(BaseModel):
    model_config = ConfigDict(extra="forbid")
    session_id: str = Field(min_length=65, max_length=65)
    generate_vlog: bool


def _validate_session_id(session_id: str) -> str:
    normalized = session_id.lower()
    if not _SESSION_PATTERN.fullmatch(normalized):
        raise HTTPException(status.HTTP_422_UNPROCESSABLE_CONTENT, "invalid session id")
    return normalized


def _validate_bmp(body: bytes) -> None:
    if len(body) != _BMP_SIZE:
        raise HTTPException(status.HTTP_422_UNPROCESSABLE_CONTENT, "invalid BMP size")
    try:
        valid = (
            body[:2] == b"BM"
            and struct.unpack_from("<I", body, 2)[0] == _BMP_SIZE
            and struct.unpack_from("<I", body, 6)[0] == 0
            and struct.unpack_from("<I", body, 10)[0] == 66
            and struct.unpack_from("<I", body, 14)[0] == 40
            and struct.unpack_from("<i", body, 18)[0] == _BMP_WIDTH
            and struct.unpack_from("<i", body, 22)[0] == _BMP_HEIGHT
            and struct.unpack_from("<H", body, 26)[0] == 1
            and struct.unpack_from("<H", body, 28)[0] == 16
            and struct.unpack_from("<I", body, 30)[0] == 3
            and struct.unpack_from("<I", body, 34)[0] == _BMP_WIDTH * _BMP_HEIGHT * 2
            and struct.unpack_from("<III", body, 54) == (0xF800, 0x07E0, 0x001F)
        )
    except struct.error as error:
        raise HTTPException(status.HTTP_422_UNPROCESSABLE_CONTENT, "invalid BMP") from error
    if not valid:
        raise HTTPException(status.HTTP_422_UNPROCESSABLE_CONTENT, "invalid BMP header")


async def _read_bmp_body(request: Request) -> bytes:
    content_length = request.headers.get("content-length")
    if content_length is not None:
        try:
            declared = int(content_length)
        except ValueError as error:
            raise HTTPException(
                status.HTTP_400_BAD_REQUEST, "invalid Content-Length"
            ) from error
        if declared != _BMP_SIZE:
            raise HTTPException(
                status.HTTP_413_CONTENT_TOO_LARGE, "BMP body must be 153666 bytes"
            )

    body = bytearray()
    async for chunk in request.stream():
        body.extend(chunk)
        if len(body) > _BMP_SIZE:
            raise HTTPException(
                status.HTTP_413_CONTENT_TOO_LARGE, "BMP body exceeds 153666 bytes"
            )
    if len(body) != _BMP_SIZE:
        raise HTTPException(status.HTTP_422_UNPROCESSABLE_CONTENT, "invalid BMP size")
    return bytes(body)


def _parse_int_header(request: Request, name: str, minimum: int, maximum: int) -> int:
    value = request.headers.get(name)
    try:
        parsed = int(value) if value is not None else None
    except ValueError as error:
        raise HTTPException(status.HTTP_422_UNPROCESSABLE_CONTENT, f"invalid {name}") from error
    if parsed is None or parsed < minimum or parsed > maximum:
        raise HTTPException(status.HTTP_422_UNPROCESSABLE_CONTENT, f"invalid {name}")
    return parsed


def _parse_float_header(request: Request, name: str) -> float:
    value = request.headers.get(name)
    try:
        parsed = float(value) if value is not None else math.nan
    except ValueError as error:
        raise HTTPException(status.HTTP_422_UNPROCESSABLE_CONTENT, f"invalid {name}") from error
    if not math.isfinite(parsed) or parsed < 0.0 or parsed > 100.0:
        raise HTTPException(status.HTTP_422_UNPROCESSABLE_CONTENT, f"invalid {name}")
    return parsed


def _require_idempotency(value: str, expected: str) -> None:
    if value != expected:
        raise HTTPException(status.HTTP_409_CONFLICT, "invalid idempotency key")


def _authorize(request: Request) -> None:
    expected = request.app.state.settings.device_token
    header = request.headers.get("Authorization", "")
    scheme, separator, token = header.partition(" ")
    if separator != " " or scheme.lower() != "bearer" or not hmac.compare_digest(
        token, expected
    ):
        raise HTTPException(
            status.HTTP_401_UNAUTHORIZED,
            "invalid bearer token",
            headers={"WWW-Authenticate": "Bearer"},
        )


def _storage(request: Request) -> Storage:
    return request.app.state.storage


@asynccontextmanager
async def _lifespan(application: FastAPI):
    settings = get_settings()
    storage = Storage(settings.database_path, settings.data_dir, settings.max_assets)
    worker = GenerationWorker(settings, storage)
    application.state.settings = settings
    application.state.storage = storage
    application.state.worker = worker
    worker.start()
    try:
        yield
    finally:
        worker.stop()


app = FastAPI(
    title="AI Vlog Server",
    version=__version__,
    docs_url=None,
    redoc_url=None,
    lifespan=_lifespan,
)


@app.get("/health")
def health() -> dict[str, Any]:
    return {"status": "ok", "version": __version__}


# Unversioned and unauthenticated so the companion App can poll the video
# list; matches the /list contract served by the on-device Android backend.
@app.get("/list")
def list_completed(
    request: Request,
    storage: Annotated[Storage, Depends(_storage)],
) -> dict[str, Any]:
    base = request.app.state.settings.public_base_url
    videos: list[dict[str, Any]] = []
    for session in storage.list_completed_sessions():
        video_path = Path(session["video_path"]) if session["video_path"] else None
        if video_path is None or not video_path.is_file():
            continue
        if video_path.stat().st_size == 0:
            continue
        session_id = session["session_id"]
        relative = f"/v1/aivlog/sessions/{session_id}/result.mp4"
        videos.append(
            {
                "session_id": session_id,
                "name": f"{session_id}.mp4",
                "title": session["title"] or "AI Vlog",
                "updated_at": session["updated_at"],
                "url": f"{base}{relative}" if base else relative,
            }
        )
    return {"videos": videos}


@app.post("/v1/aivlog/sessions")
def create_session(
    payload: SessionCreate,
    request: Request,
    idempotency_key: Annotated[str, Header(alias="Idempotency-Key")],
    storage: Annotated[Storage, Depends(_storage)],
    _: Annotated[None, Depends(_authorize)],
) -> JSONResponse:
    session_id = _validate_session_id(payload.session_id)
    device_id = payload.device_id.lower()
    if not session_id.startswith(f"{device_id}-"):
        raise HTTPException(status.HTTP_422_UNPROCESSABLE_CONTENT, "device id mismatch")
    _require_idempotency(idempotency_key, f"aivlog-{session_id}-start")
    try:
        session, created = storage.create_session(
            session_id, device_id, payload.score_threshold
        )
    except ValueError as error:
        raise HTTPException(status.HTTP_409_CONFLICT, str(error)) from error
    return JSONResponse(
        {"session_id": session_id, "status": session["state"].lower()},
        status_code=status.HTTP_201_CREATED if created else status.HTTP_200_OK,
    )


@app.put("/v1/aivlog/sessions/{session_id}/assets/{sequence}")
async def upload_asset(
    session_id: str,
    sequence: int,
    request: Request,
    idempotency_key: Annotated[str, Header(alias="Idempotency-Key")],
    storage: Annotated[Storage, Depends(_storage)],
    _: Annotated[None, Depends(_authorize)],
) -> JSONResponse:
    session_id = _validate_session_id(session_id)
    if sequence < 1 or sequence > request.app.state.settings.max_assets:
        raise HTTPException(status.HTTP_422_UNPROCESSABLE_CONTENT, "invalid sequence")
    _require_idempotency(idempotency_key, f"aivlog-{session_id}-{sequence:04d}")
    if request.headers.get("content-type", "").split(";", 1)[0].lower() != "image/bmp":
        raise HTTPException(status.HTTP_415_UNSUPPORTED_MEDIA_TYPE, "image/bmp required")
    score = _parse_int_header(request, "X-AIVlog-Score", 0, 100)
    raw_score = _parse_float_header(request, "X-AIVlog-Raw-Score")
    dhash = request.headers.get("X-AIVlog-DHash", "")
    if not _HEX_16_PATTERN.fullmatch(dhash):
        raise HTTPException(status.HTTP_422_UNPROCESSABLE_CONTENT, "invalid dHash")
    body = await _read_bmp_body(request)
    _validate_bmp(body)
    try:
        _, created = storage.store_asset(
            session_id, sequence, score, raw_score, dhash, body
        )
    except KeyError as error:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "session not found") from error
    except OverflowError as error:
        raise HTTPException(status.HTTP_413_CONTENT_TOO_LARGE, str(error)) from error
    except ValueError as error:
        raise HTTPException(status.HTTP_409_CONFLICT, str(error)) from error
    return JSONResponse(
        {"session_id": session_id, "asset": sequence, "status": "stored"},
        status_code=status.HTTP_201_CREATED if created else status.HTTP_200_OK,
    )


@app.post("/v1/aivlog/sessions/{session_id}/finish")
def finish_session(
    session_id: str,
    payload: SessionFinish,
    request: Request,
    idempotency_key: Annotated[str, Header(alias="Idempotency-Key")],
    storage: Annotated[Storage, Depends(_storage)],
    _: Annotated[None, Depends(_authorize)],
) -> JSONResponse:
    session_id = _validate_session_id(session_id)
    if payload.session_id.lower() != session_id or not payload.generate_vlog:
        raise HTTPException(status.HTTP_422_UNPROCESSABLE_CONTENT, "invalid finish request")
    _require_idempotency(idempotency_key, f"aivlog-{session_id}-finish")
    try:
        session = storage.finish_session(session_id)
    except KeyError as error:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "session not found") from error
    request.app.state.worker.notify()
    return JSONResponse(
        {"session_id": session_id, "status": session["state"].lower()},
        status_code=status.HTTP_202_ACCEPTED,
    )


@app.get("/v1/aivlog/sessions/{session_id}")
def session_status(
    session_id: str,
    request: Request,
    storage: Annotated[Storage, Depends(_storage)],
    _: Annotated[None, Depends(_authorize)],
) -> dict[str, Any]:
    session_id = _validate_session_id(session_id)
    try:
        session = storage.get_session(session_id)
    except KeyError as error:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "session not found") from error
    result: dict[str, Any] = {
        "session_id": session_id,
        "status": session["state"].lower(),
        "asset_count": session["asset_count"],
        "attempts": session["attempts"],
    }
    if session.get("title"):
        result["title"] = session["title"]
    if session.get("storyboard"):
        result["storyboard"] = session["storyboard"]
    if session.get("error"):
        result["error"] = session["error"]
    if session["state"] == "COMPLETED":
        relative = f"/v1/aivlog/sessions/{session_id}/result.mp4"
        base = request.app.state.settings.public_base_url
        result["video_url"] = f"{base}{relative}" if base else relative
    return result


@app.get("/v1/aivlog/sessions/{session_id}/result.mp4")
def download_result(
    session_id: str,
    request: Request,
    storage: Annotated[Storage, Depends(_storage)],
    _: Annotated[None, Depends(_authorize)],
) -> FileResponse:
    session_id = _validate_session_id(session_id)
    try:
        session = storage.get_session(session_id)
    except KeyError as error:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "session not found") from error
    if session["state"] != "COMPLETED" or not session.get("video_path"):
        raise HTTPException(status.HTTP_409_CONFLICT, "Vlog is not ready")
    video_path = Path(session["video_path"])
    if not video_path.is_file() or video_path.stat().st_size == 0:
        raise HTTPException(
            status.HTTP_503_SERVICE_UNAVAILABLE,
            "published Vlog is missing; restart the service to recover",
        )
    return FileResponse(
        video_path, media_type="video/mp4", filename=f"{session_id}.mp4"
    )
