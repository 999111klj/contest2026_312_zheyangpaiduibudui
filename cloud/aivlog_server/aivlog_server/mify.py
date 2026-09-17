"""MiFY mimo-v2.5 storyboard generation."""

from __future__ import annotations

import base64
import json
import logging
import time
from pathlib import Path
from typing import Any
import urllib.error
import urllib.request

from .config import Settings

_LOG = logging.getLogger(__name__)

_MOTIONS = ("zoom_in", "zoom_out", "pan_left", "pan_right")
_TRANSITIONS = ("fade", "dissolve", "smoothleft", "smoothright", "fadeblack")
_STORYBOARD_ATTEMPTS = 3


def _editing_defaults(index: int) -> tuple[str, str]:
    """Return deterministic motion/transition defaults for legacy storyboards."""
    return _MOTIONS[index % len(_MOTIONS)], _TRANSITIONS[index % len(_TRANSITIONS)]


def _fallback_storyboard(
    assets: list[dict[str, Any]], max_selected: int, error: str | None = None
) -> dict[str, Any]:
    selected = sorted(assets, key=lambda item: (-item["score"], item["sequence"]))[
        :max_selected
    ]
    selected.sort(key=lambda item: item["sequence"])
    timeline: list[dict[str, Any]] = []
    for index, item in enumerate(selected):
        motion, transition = _editing_defaults(index)
        timeline.append(
            {
                "asset": item["sequence"],
                "duration": 2.6,
                "caption": f"精选画面 {item['sequence']}",
                "motion": motion,
                "transition": transition,
            }
        )
    storyboard: dict[str, Any] = {
        "title": "AI Vlog",
        "source": "local-score-fallback",
        "timeline": timeline,
    }
    if error:
        storyboard["model_error"] = error[:300]
    return storyboard


def _validate_storyboard(
    candidate: Any, assets: list[dict[str, Any]], max_selected: int
) -> dict[str, Any]:
    if not isinstance(candidate, dict):
        raise ValueError("storyboard is not an object")
    title = candidate.get("title", "AI Vlog")
    if not isinstance(title, str) or not title.strip():
        title = "AI Vlog"
    title = " ".join(title.split())[:40]

    available = {item["sequence"] for item in assets}
    timeline: list[dict[str, Any]] = []
    used: set[int] = set()
    for item in candidate.get("timeline", []):
        if not isinstance(item, dict):
            continue
        try:
            sequence = int(item["asset"])
            duration = float(item.get("duration", 2.6))
        except (KeyError, TypeError, ValueError):
            continue
        if sequence not in available or sequence in used:
            continue

        duration = min(4.0, max(2.0, duration))
        caption = item.get("caption", "")
        if not isinstance(caption, str):
            caption = ""
        caption = " ".join(caption.split())[:40]

        default_motion, default_transition = _editing_defaults(len(timeline))
        motion = item.get("motion", default_motion)
        transition = item.get("transition", default_transition)
        if motion not in _MOTIONS:
            motion = default_motion
        if transition not in _TRANSITIONS:
            transition = default_transition

        timeline.append(
            {
                "asset": sequence,
                "duration": round(duration, 2),
                "caption": caption,
                "motion": motion,
                "transition": transition,
            }
        )
        used.add(sequence)
        if len(timeline) >= max_selected:
            break
    if not timeline:
        raise ValueError("storyboard contains no valid assets")
    return {"title": title, "source": "mimo-v2.5", "timeline": timeline}


def _call_mify(settings: Settings, assets: list[dict[str, Any]]) -> dict[str, Any]:
    content: list[dict[str, Any]] = [
        {
            "type": "text",
            "text": (
                "你是专业短视频Vlog剪辑师。根据图片内容、拍摄顺序和端侧评分，"
                "选择有差异、有叙事价值的素材，避免连续选择近似画面，形成开场、"
                "发展和收束。最多选择"
                f"{settings.max_selected_assets}张，不必选满。只返回JSON："
                '{"title":"...","timeline":[{"asset":1,"duration":2.6,'
                '"caption":"...","motion":"zoom_in",'
                '"transition":"fade"}]}。asset必须使用提供的序号且不得重复；'
                "duration范围2到4秒；caption使用简洁中文且不超过18个汉字；"
                "motion只能是zoom_in、zoom_out、pan_left、pan_right；"
                "transition只能是fade、dissolve、smoothleft、smoothright、"
                "fadeblack。运镜和转场应交替使用并匹配画面内容。"
            ),
        }
    ]
    for asset in assets:
        content.append(
            {
                "type": "text",
                "text": (
                    f"asset={asset['sequence']} score={asset['score']} "
                    f"raw_score={asset['raw_score']:.6f}"
                ),
            }
        )
        encoded = base64.b64encode(Path(asset["path"]).read_bytes()).decode("ascii")
        content.append(
            {
                "type": "image_url",
                "image_url": {
                    "url": f"data:image/bmp;base64,{encoded}",
                    "detail": "low",
                },
            }
        )

    payload = {
        "model": settings.mify_model,
        "messages": [{"role": "user", "content": content}],
        "response_format": {"type": "json_object"},
        "temperature": 0,
        "max_tokens": 2048,
    }
    request = urllib.request.Request(
        f"{settings.mify_base_url}/chat/completions",
        data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
        headers={
            "Authorization": f"Bearer {settings.mify_api_key}",
            "X-Model-Provider-Id": settings.mify_provider,
            "Content-Type": "application/json",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(
            request, timeout=settings.mify_timeout_seconds
        ) as response:
            body = response.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as error:
        detail = error.read().decode("utf-8", "replace")[:500]
        raise RuntimeError(f"MiFY HTTP {error.code}: {detail}") from error
    data = json.loads(body)
    choices = data.get("choices") or []
    if not choices:
        raise RuntimeError("MiFY returned no choices")
    content_text = (choices[0].get("message") or {}).get("content")
    if not isinstance(content_text, str):
        raise RuntimeError("MiFY returned no message content")
    return json.loads(content_text)


def build_storyboard(
    settings: Settings, assets: list[dict[str, Any]]
) -> dict[str, Any]:
    """Return a validated storyboard, falling back deterministically."""
    if not assets:
        raise ValueError("cannot generate a Vlog without assets")
    if not settings.mify_api_key:
        return _fallback_storyboard(
            assets, settings.max_selected_assets, "MIFY_API_KEY is not configured"
        )
    # mimo-v2.5 occasionally emits malformed JSON despite response_format;
    # retry the call before giving up on real captions.
    error: Exception | None = None
    for attempt in range(_STORYBOARD_ATTEMPTS):
        try:
            candidate = _call_mify(settings, assets)
            return _validate_storyboard(candidate, assets, settings.max_selected_assets)
        except Exception as failure:  # The local-score path must remain available offline.
            error = failure
            _LOG.warning(
                "storyboard attempt %d/%d failed: %s: %s",
                attempt + 1,
                _STORYBOARD_ATTEMPTS,
                type(error).__name__,
                error,
            )
            if attempt + 1 < _STORYBOARD_ATTEMPTS:
                time.sleep(1.0)
    return _fallback_storyboard(
        assets, settings.max_selected_assets, f"{type(error).__name__}: {error}"
    )
