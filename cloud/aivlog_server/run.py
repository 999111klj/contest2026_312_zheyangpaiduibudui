"""AI Vlog server entry point."""

from aivlog_server.config import get_settings
import uvicorn


if __name__ == "__main__":
    settings = get_settings()
    uvicorn.run(
        "aivlog_server.app:app",
        host=settings.host,
        port=settings.port,
        ssl_certfile=str(settings.tls_certfile) if settings.tls_certfile else None,
        ssl_keyfile=str(settings.tls_keyfile) if settings.tls_keyfile else None,
        workers=1,
        proxy_headers=True,
        forwarded_allow_ips="127.0.0.1",
    )
