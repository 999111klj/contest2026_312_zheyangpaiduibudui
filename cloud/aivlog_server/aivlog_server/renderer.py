"""Cinematic FFmpeg renderer for AI-selected still images."""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
from typing import Any

from .config import Settings

_WIDTH = 1280
_HEIGHT = 720
_FPS = 30
_TRANSITION_SECONDS = 0.55
_FONT_CANDIDATES = (
    Path("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"),
    Path("/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc"),
    Path("/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc"),
)
_MOTIONS = ("zoom_in", "zoom_out", "pan_left", "pan_right")
_TRANSITIONS = ("fade", "dissolve", "smoothleft", "smoothright", "fadeblack")


def _sync_directory(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _atomic_text(path: Path, content: str) -> None:
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    with temporary.open("w", encoding="utf-8") as stream:
        stream.write(content)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)
    _sync_directory(path.parent)


def _font_file() -> Path:
    configured = os.environ.get("AIVLOG_FONT_FILE", "").strip()
    candidates = (Path(configured).expanduser(),) if configured else _FONT_CANDIDATES
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    raise RuntimeError(
        "no Chinese font found; set AIVLOG_FONT_FILE to a readable TTF/TTC file"
    )


def _filter_path(path: Path) -> str:
    """Escape a path embedded in a quoted FFmpeg filter option."""
    return (
        str(path.resolve())
        .replace("\\", "\\\\")
        .replace(":", "\\:")
        .replace("'", "\\'")
        .replace("[", "\\[")
        .replace("]", "\\]")
    )


def _ass_escape(value: str) -> str:
    """Make model text inert when used as ASS dialogue text."""
    return (
        value.replace("\\", "\\\\")
        .replace("{", "（")
        .replace("}", "）")
        .replace("\r\n", "\\N")
        .replace("\r", "\\N")
        .replace("\n", "\\N")
    )


def _ass_time(seconds: float) -> str:
    centiseconds = max(0, round(seconds * 100))
    hours, remainder = divmod(centiseconds, 360000)
    minutes, remainder = divmod(remainder, 6000)
    whole_seconds, fraction = divmod(remainder, 100)
    return f"{hours}:{minutes:02d}:{whole_seconds:02d}.{fraction:02d}"


def _subtitle_document(
    title: str, timeline: list[dict[str, Any]], transition: float
) -> str:
    starts: list[float] = []
    cursor = 0.0
    for index, item in enumerate(timeline):
        starts.append(cursor)
        cursor += float(item["duration"])
        if index + 1 < len(timeline):
            cursor -= transition
    total_duration = cursor

    lines = [
        "[Script Info]",
        "ScriptType: v4.00+",
        f"PlayResX: {_WIDTH}",
        f"PlayResY: {_HEIGHT}",
        "WrapStyle: 2",
        "ScaledBorderAndShadow: yes",
        "YCbCr Matrix: TV.709",
        "",
        "[V4+ Styles]",
        (
            "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
            "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, "
            "ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
            "Alignment, MarginL, MarginR, MarginV, Encoding"
        ),
        (
            "Style: Title,Noto Sans CJK SC,48,&H00FFFFFF,&H000000FF,"
            "&H78000000,&H78000000,-1,0,0,0,100,100,1,0,3,1,0,8,80,80,46,1"
        ),
        (
            "Style: Caption,Noto Sans CJK SC,34,&H00FFFFFF,&H000000FF,"
            "&H78000000,&H78000000,0,0,0,0,100,100,0,0,3,1,0,2,80,80,48,1"
        ),
        "",
        "[Events]",
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text",
    ]

    clean_title = _ass_escape(" ".join(str(title or "AI Vlog").split())[:40])
    title_end = min(2.2, max(0.8, total_duration - 0.1))
    lines.append(
        f"Dialogue: 1,{_ass_time(0.2)},{_ass_time(title_end)},"
        f"Title,,0,0,0,,{clean_title}"
    )

    for index, item in enumerate(timeline):
        caption = _ass_escape(" ".join(str(item.get("caption", "")).split())[:40])
        if not caption:
            continue
        start = starts[index] + (transition * 0.6 if index else 0.35)
        end = starts[index] + float(item["duration"])
        end -= transition * 0.6 if index + 1 < len(timeline) else 0.35
        if end <= start:
            continue
        lines.append(
            f"Dialogue: 0,{_ass_time(start)},{_ass_time(end)},"
            f"Caption,,0,0,0,,{caption}"
        )

    return "\n".join(lines) + "\n"


def _motion_filter(motion: str, frames: int) -> str:
    denominator = max(1, frames - 1)
    if motion == "zoom_out":
        zoom = f"max(1.08-on*0.08/{denominator},1.0)"
        x = "iw/2-(iw/zoom/2)"
    elif motion == "pan_left":
        zoom = "1.08"
        x = f"(iw-iw/zoom)*(1-on/{denominator})"
    elif motion == "pan_right":
        zoom = "1.08"
        x = f"(iw-iw/zoom)*on/{denominator}"
    else:
        zoom = f"min(1.0+on*0.08/{denominator},1.08)"
        x = "iw/2-(iw/zoom/2)"
    y = "ih/2-(ih/zoom/2)"
    return (
        f"zoompan=z='{zoom}':x='{x}':y='{y}':d=1:"
        f"s={_WIDTH}x{_HEIGHT}:fps={_FPS}"
    )


def _run_ffmpeg(command: list[str], deadline: float, stage: str) -> None:
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise RuntimeError("FFmpeg rendering timed out")
    try:
        result = subprocess.run(
            command,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=remaining,
        )
    except subprocess.TimeoutExpired as error:
        raise RuntimeError("FFmpeg rendering timed out") from error
    if result.returncode != 0:
        raise RuntimeError(f"FFmpeg {stage} failed: {result.stderr[-2000:]}")


def _segment_filter(item: dict[str, Any]) -> str:
    duration = float(item["duration"])
    frames = max(1, round(duration * _FPS))
    return ";".join(
        [
            "[0:v]split=2[bg][fg]",
            (
                f"[bg]scale={_WIDTH}:{_HEIGHT}:"
                "force_original_aspect_ratio=increase,"
                f"crop={_WIDTH}:{_HEIGHT},boxblur=24:2,"
                "eq=brightness=-0.12:saturation=1.12[bgp]"
            ),
            (
                "[fg]scale=1120:680:force_original_aspect_ratio=decrease,"
                "eq=contrast=1.04:saturation=1.06[fgp]"
            ),
            (
                "[bgp][fgp]overlay=(W-w)/2:(H-h)/2:shortest=1,"
                "format=yuv420p[base]"
            ),
            (
                f"[base]{_motion_filter(str(item['motion']), frames)},"
                f"trim=duration={duration:.2f},fps={_FPS},setsar=1,settb=AVTB,"
                f"setpts=N/({_FPS}*TB),format=yuv420p[outv]"
            ),
        ]
    )


def render_vlog(
    settings: Settings,
    session_id: str,
    storyboard: dict[str, Any],
    assets: list[dict[str, Any]],
) -> Path:
    """Render a motion slideshow with transitions and burned-in model text."""
    ffmpeg = shutil.which(settings.ffmpeg_binary)
    if ffmpeg is None:
        raise RuntimeError(f"FFmpeg executable not found: {settings.ffmpeg_binary}")

    by_sequence = {asset["sequence"]: asset for asset in assets}
    selected: list[dict[str, Any]] = []
    for index, raw_item in enumerate(storyboard["timeline"]):
        asset = by_sequence.get(int(raw_item["asset"]))
        if asset is None:
            continue
        motion = raw_item.get("motion", _MOTIONS[index % len(_MOTIONS)])
        transition = raw_item.get(
            "transition", _TRANSITIONS[index % len(_TRANSITIONS)]
        )
        selected.append(
            {
                "path": Path(asset["path"]),
                "asset": int(raw_item["asset"]),
                "duration": min(4.0, max(2.0, float(raw_item["duration"]))),
                "caption": str(raw_item.get("caption", "")),
                "motion": motion if motion in _MOTIONS else _MOTIONS[index % len(_MOTIONS)],
                "transition": (
                    transition
                    if transition in _TRANSITIONS
                    else _TRANSITIONS[index % len(_TRANSITIONS)]
                ),
            }
        )
    if not selected:
        raise RuntimeError("storyboard selected no available assets")

    session_dir = settings.data_dir / "sessions" / session_id
    session_dir.mkdir(parents=True, exist_ok=True)
    storyboard_path = session_dir / "storyboard.json"
    subtitle_path = session_dir / "subtitles.ass"
    output_path = session_dir / "vlog.mp4"
    temporary_output = session_dir / ".vlog.tmp.mp4"
    (session_dir / "timeline.ffconcat").unlink(missing_ok=True)

    normalized_storyboard = dict(storyboard)
    normalized_storyboard["timeline"] = [
        {
            "asset": item["asset"],
            "duration": item["duration"],
            "caption": item["caption"],
            "motion": item["motion"],
            "transition": item["transition"],
        }
        for item in selected
    ]
    _atomic_text(
        storyboard_path,
        json.dumps(normalized_storyboard, ensure_ascii=False, indent=2) + "\n",
    )
    _atomic_text(
        subtitle_path,
        _subtitle_document(
            str(storyboard.get("title", "AI Vlog")),
            normalized_storyboard["timeline"],
            _TRANSITION_SECONDS,
        ),
    )

    deadline = time.monotonic() + settings.render_timeout_seconds
    temporary_output.unlink(missing_ok=True)
    try:
        with tempfile.TemporaryDirectory(prefix=".render-", dir=session_dir) as work:
            work_dir = Path(work)
            segment_paths: list[Path] = []
            for index, item in enumerate(selected):
                segment_path = work_dir / f"segment-{index:04d}.mp4"
                duration = float(item["duration"])
                segment_command = [
                    ffmpeg,
                    "-hide_banner",
                    "-loglevel",
                    "warning",
                    "-y",
                    "-loop",
                    "1",
                    "-framerate",
                    str(_FPS),
                    "-t",
                    f"{duration:.2f}",
                    "-i",
                    str(item["path"]),
                    "-filter_complex",
                    _segment_filter(item),
                    "-map",
                    "[outv]",
                    "-an",
                    "-t",
                    f"{duration:.2f}",
                    "-c:v",
                    "libx264",
                    "-preset",
                    "fast",
                    "-crf",
                    "16",
                    "-pix_fmt",
                    "yuv420p",
                    "-r",
                    str(_FPS),
                    "-fps_mode",
                    "cfr",
                    "-video_track_timescale",
                    str(_FPS * 1000),
                    str(segment_path),
                ]
                _run_ffmpeg(segment_command, deadline, f"segment {index + 1}")
                if not segment_path.is_file() or segment_path.stat().st_size == 0:
                    raise RuntimeError(
                        f"FFmpeg produced an empty segment {index + 1}"
                    )
                segment_paths.append(segment_path)

            transition_paths: list[Path] = []
            transition_expressions = {
                "fade": (
                    f"A*(1-T/{_TRANSITION_SECONDS})+"
                    f"B*(T/{_TRANSITION_SECONDS})"
                ),
                "dissolve": (
                    "if(lte(mod(floor(X/12)*37+floor(Y/12)*17,101)/101,"
                    f"T/{_TRANSITION_SECONDS}),B,A)"
                ),
                "smoothleft": (
                    "A*(1-clip((X-W*(1-T/"
                    f"{_TRANSITION_SECONDS}))/64,0,1))+"
                    "B*clip((X-W*(1-T/"
                    f"{_TRANSITION_SECONDS}))/64,0,1)"
                ),
                "smoothright": (
                    "A*(1-clip((W*T/"
                    f"{_TRANSITION_SECONDS}-X)/64,0,1))+"
                    "B*clip((W*T/"
                    f"{_TRANSITION_SECONDS}-X)/64,0,1)"
                ),
                "fadeblack": (
                    f"if(lt(T,{_TRANSITION_SECONDS / 2}),"
                    f"A*(1-T/{_TRANSITION_SECONDS / 2}),"
                    f"B*((T-{_TRANSITION_SECONDS / 2})/"
                    f"{_TRANSITION_SECONDS / 2}))"
                ),
            }
            for index in range(len(segment_paths) - 1):
                transition_path = work_dir / f"transition-{index:04d}.mp4"
                previous_duration = float(selected[index]["duration"])
                transition_name = str(selected[index]["transition"])
                expression = transition_expressions[transition_name]
                transition_filter = ";".join(
                    [
                        (
                            f"[0:v]trim=start={previous_duration - _TRANSITION_SECONDS:.2f}:"
                            f"end={previous_duration:.2f},setpts=PTS-STARTPTS,"
                            "tpad=stop_mode=clone:stop_duration=0.10,"
                            f"trim=duration={_TRANSITION_SECONDS:.2f},"
                            f"fps={_FPS},setsar=1,format=yuv444p[a]"
                        ),
                        (
                            f"[1:v]trim=start=0:end={_TRANSITION_SECONDS:.2f},"
                            "setpts=PTS-STARTPTS,"
                            "tpad=stop_mode=clone:stop_duration=0.10,"
                            f"trim=duration={_TRANSITION_SECONDS:.2f},"
                            f"fps={_FPS},setsar=1,format=yuv444p[b]"
                        ),
                        (
                            f"[a][b]blend=all_expr='{expression}':shortest=1,"
                            f"fps={_FPS},setsar=1,settb=AVTB,"
                            f"setpts=N/({_FPS}*TB),format=yuv420p[outv]"
                        ),
                    ]
                )
                transition_command = [
                    ffmpeg,
                    "-hide_banner",
                    "-loglevel",
                    "warning",
                    "-y",
                    "-i",
                    str(segment_paths[index]),
                    "-i",
                    str(segment_paths[index + 1]),
                    "-filter_complex",
                    transition_filter,
                    "-map",
                    "[outv]",
                    "-an",
                    "-t",
                    f"{_TRANSITION_SECONDS:.2f}",
                    "-c:v",
                    "libx264",
                    "-preset",
                    "fast",
                    "-crf",
                    "16",
                    "-pix_fmt",
                    "yuv420p",
                    "-r",
                    str(_FPS),
                    "-fps_mode",
                    "cfr",
                    "-video_track_timescale",
                    str(_FPS * 1000),
                    str(transition_path),
                ]
                _run_ffmpeg(
                    transition_command, deadline, f"transition {index + 1}"
                )
                if not transition_path.is_file() or transition_path.stat().st_size == 0:
                    raise RuntimeError(
                        f"FFmpeg produced an empty transition {index + 1}"
                    )
                transition_paths.append(transition_path)

            command = [ffmpeg, "-hide_banner", "-loglevel", "warning", "-y"]
            for media_path in segment_paths + transition_paths:
                command.extend(["-i", str(media_path)])

            filters: list[str] = []
            ordered_labels: list[str] = []
            last_segment = len(segment_paths) - 1
            for index, item in enumerate(selected):
                body_start = 0.0 if index == 0 else _TRANSITION_SECONDS
                body_end = float(item["duration"])
                if index != last_segment:
                    body_end -= _TRANSITION_SECONDS
                body_duration = body_end - body_start
                body_label = f"body{index}"
                filters.append(
                    f"[{index}:v]trim=start={body_start:.2f}:end={body_end:.2f},"
                    "setpts=PTS-STARTPTS,tpad=stop_mode=clone:stop_duration=0.10,"
                    f"trim=duration={body_duration:.2f},fps={_FPS},"
                    f"setsar=1,settb=AVTB,setpts=N/({_FPS}*TB),"
                    f"format=yuv420p[{body_label}]"
                )
                ordered_labels.append(body_label)
                if index < len(transition_paths):
                    transition_input = len(segment_paths) + index
                    transition_label = f"transition{index}"
                    filters.append(
                        f"[{transition_input}:v]tpad=stop_mode=clone:"
                        "stop_duration=0.10,"
                        f"trim=duration={_TRANSITION_SECONDS:.2f},fps={_FPS},"
                        f"setsar=1,settb=AVTB,setpts=N/({_FPS}*TB),"
                        f"format=yuv420p[{transition_label}]"
                    )
                    ordered_labels.append(transition_label)

            concat_inputs = "".join(f"[{label}]" for label in ordered_labels)
            filters.append(
                f"{concat_inputs}concat=n={len(ordered_labels)}:v=1:a=0[sequence]"
            )
            current_duration = sum(float(item["duration"]) for item in selected)
            current_duration -= _TRANSITION_SECONDS * (len(selected) - 1)
            font = _font_file()
            fade_out_start = max(0.0, current_duration - 0.45)
            filters.append(
                f"[sequence]fade=t=in:st=0:d=0.35,"
                f"fade=t=out:st={fade_out_start:.2f}:d=0.45,"
                f"subtitles=filename='{_filter_path(subtitle_path)}':"
                f"fontsdir='{_filter_path(font.parent)}':"
                f"original_size={_WIDTH}x{_HEIGHT},format=yuv420p[outv]"
            )
            command.extend(
                [
                    "-filter_complex",
                    ";".join(filters),
                    "-map",
                    "[outv]",
                    "-an",
                    "-t",
                    f"{current_duration:.2f}",
                    "-c:v",
                    "libx264",
                    "-preset",
                    "medium",
                    "-crf",
                    "20",
                    "-pix_fmt",
                    "yuv420p",
                    "-r",
                    str(_FPS),
                    "-fps_mode",
                    "cfr",
                    "-movflags",
                    "+faststart",
                    "-metadata",
                    f"title={storyboard.get('title', 'AI Vlog')}",
                    str(temporary_output),
                ]
            )
            _run_ffmpeg(command, deadline, "final composition")
    except Exception:
        temporary_output.unlink(missing_ok=True)
        raise

    if not temporary_output.is_file() or temporary_output.stat().st_size == 0:
        raise RuntimeError("FFmpeg produced an empty output")
    with temporary_output.open("rb") as stream:
        os.fsync(stream.fileno())
    os.replace(temporary_output, output_path)
    _sync_directory(session_dir)
    return output_path
