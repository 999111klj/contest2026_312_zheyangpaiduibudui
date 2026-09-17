#!/usr/bin/env python3
"""Receive lossless BMP uploads from the camera gallery over HTTP."""

import argparse
import hashlib
import json
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import re
import tempfile
from urllib.parse import urlsplit

EXPECTED_BMP_SIZE = 66 + 320 * 240 * 2
FILENAME_PATTERN = re.compile(r"/upload/(IMG_[0-9]{4}\.BMP)")
SHA256_PATTERN = re.compile(r"[0-9a-f]{64}")


class PhotoReceiver(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    output_directory: Path

    def send_json(
        self,
        status: int,
        payload: dict[str, object],
        headers: dict[str, str] | None = None,
    ) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        if headers is not None:
            for name, value in headers.items():
                self.send_header(name, value)
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        self.connection.settimeout(30)
        match = FILENAME_PATTERN.fullmatch(urlsplit(self.path).path)
        if match is None:
            self.send_json(404, {"error": "expected /upload/IMG_NNNN.BMP"})
            return

        if self.headers.get_content_type() != "image/bmp":
            self.send_json(415, {"error": "Content-Type must be image/bmp"})
            return

        try:
            content_length = int(self.headers.get("Content-Length", ""))
        except ValueError:
            content_length = -1

        if content_length != EXPECTED_BMP_SIZE:
            self.send_json(
                413,
                {"error": f"BMP must be exactly {EXPECTED_BMP_SIZE} bytes"},
            )
            return

        expected_digest = self.headers.get("X-Content-SHA256", "").lower()
        if SHA256_PATTERN.fullmatch(expected_digest) is None:
            self.send_json(400, {"error": "missing or invalid SHA-256 header"})
            return

        filename = match.group(1)
        temporary_path: Path | None = None
        digest = hashlib.sha256()
        remaining = content_length
        try:
            with tempfile.NamedTemporaryFile(
                mode="wb",
                prefix=f".{filename}.",
                suffix=".part",
                dir=self.output_directory,
                delete=False,
            ) as output:
                temporary_path = Path(output.name)
                while remaining:
                    block = self.rfile.read(min(65536, remaining))
                    if not block:
                        raise ConnectionError("upload ended before Content-Length")
                    output.write(block)
                    digest.update(block)
                    remaining -= len(block)
                output.flush()
                os.fsync(output.fileno())

            actual_digest = digest.hexdigest()
            if actual_digest != expected_digest:
                temporary_path.unlink(missing_ok=True)
                self.send_json(
                    422,
                    {
                        "error": "SHA-256 mismatch",
                        "expected": expected_digest,
                        "actual": actual_digest,
                    },
                )
                return

            with temporary_path.open("rb") as uploaded:
                if uploaded.read(2) != b"BM":
                    temporary_path.unlink(missing_ok=True)
                    self.send_json(422, {"error": "invalid BMP signature"})
                    return

            destination = self.output_directory / filename
            os.replace(temporary_path, destination)
            temporary_path = None
            directory_fd = os.open(self.output_directory, os.O_RDONLY)
            try:
                os.fsync(directory_fd)
            finally:
                os.close(directory_fd)
            self.send_json(
                201,
                {
                    "file": str(destination),
                    "bytes": content_length,
                    "sha256": actual_digest,
                },
                {"X-Content-SHA256": actual_digest},
            )
            print(
                f"received {destination} ({content_length} bytes) "
                f"sha256={actual_digest}",
                flush=True,
            )
        except (ConnectionError, OSError) as error:
            if temporary_path is not None:
                temporary_path.unlink(missing_ok=True)
            self.send_json(500, {"error": str(error)})

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        self.send_json(
            200,
            {
                "service": "camera-gallery-photo-receiver",
                "upload": "/upload/IMG_NNNN.BMP",
                "bytes": EXPECTED_BMP_SIZE,
            },
        )


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Receive ESP32-S3-EYE camera_gallery BMP uploads"
    )
    parser.add_argument("--bind", default="0.0.0.0", help="listen address")
    parser.add_argument("--port", type=int, default=8080, help="listen port")
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("received_photos"),
        help="directory for received BMP files",
    )
    return parser.parse_args()


def main() -> None:
    arguments = parse_arguments()
    if not 1 <= arguments.port <= 65535:
        raise SystemExit("port must be between 1 and 65535")

    arguments.output.mkdir(parents=True, exist_ok=True)
    PhotoReceiver.output_directory = arguments.output.resolve()
    server = ThreadingHTTPServer((arguments.bind, arguments.port), PhotoReceiver)
    print(
        f"listening on http://{arguments.bind}:{arguments.port}; "
        f"saving to {PhotoReceiver.output_directory}",
        flush=True,
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
