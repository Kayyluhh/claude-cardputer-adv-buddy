"""Unix-socket ND-JSON server: hook scripts and skills connect here."""
from __future__ import annotations

import asyncio
import json
import logging
from pathlib import Path
from typing import Awaitable, Callable

log = logging.getLogger(__name__)

# Handler signature: (msg, respond) where respond is an async callable accepting a dict.
Handler = Callable[[dict, Callable[[dict], Awaitable[None]]], Awaitable[None]]


class HookServer:
    def __init__(self, sock_path: Path, handler: Handler) -> None:
        self.sock_path = sock_path
        self._handler = handler
        self._server: asyncio.AbstractServer | None = None

    async def start(self) -> None:
        # Remove stale socket if present.
        if self.sock_path.exists():
            self.sock_path.unlink()
        self.sock_path.parent.mkdir(parents=True, exist_ok=True)
        self._server = await asyncio.start_unix_server(self._on_conn, path=str(self.sock_path))
        log.info("hook server listening on %s", self.sock_path)

    async def stop(self) -> None:
        if self._server is not None:
            self._server.close()
            await self._server.wait_closed()
            self._server = None
        if self.sock_path.exists():
            self.sock_path.unlink()

    async def _on_conn(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            line = await reader.readline()
            if not line:
                return
            try:
                msg = json.loads(line)
            except json.JSONDecodeError:
                log.warning("hook server: bad JSON from client; closing")
                return

            async def respond(payload: dict) -> None:
                writer.write((json.dumps(payload) + "\n").encode("utf-8"))
                await writer.drain()

            await self._handler(msg, respond)
        except ConnectionError:
            pass
        except Exception:
            log.exception("hook server: handler error")
        finally:
            try:
                writer.close()
                await writer.wait_closed()
            except Exception:
                pass
