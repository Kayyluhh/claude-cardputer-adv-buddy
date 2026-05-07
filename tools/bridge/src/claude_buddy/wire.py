"""NUS protocol constants and line framing helpers."""
from __future__ import annotations

import json
from typing import Any

# Nordic UART Service UUIDs from REFERENCE.md §Transport.
NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # desktop -> device, write
NUS_TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # device -> desktop, notify

# Turn-event UTF-8 byte cap from REFERENCE.md §Turn events.
TURN_EVENT_BYTE_CAP = 4096


def encode_line(obj: Any) -> bytes:
    """Serialize obj as compact JSON terminated with a newline, UTF-8 encoded."""
    return (json.dumps(obj, separators=(",", ":"), ensure_ascii=False) + "\n").encode("utf-8")


def turn_event_too_large(evt: Any) -> bool:
    """True if the encoded turn event (without trailing newline) exceeds 4KB UTF-8."""
    encoded = json.dumps(evt, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    return len(encoded) > TURN_EVENT_BYTE_CAP


class LineBuffer:
    """Accumulates byte chunks and yields complete \\n-terminated lines.

    The BLE notification layer fragments at the MTU boundary; this reassembles.
    The Unix socket also uses ND-JSON.
    """

    def __init__(self) -> None:
        self._buf = bytearray()

    def feed(self, chunk: bytes) -> list[bytes]:
        """Append chunk, return any complete lines (without trailing newline). Skips empty lines."""
        self._buf.extend(chunk)
        out: list[bytes] = []
        while b"\n" in self._buf:
            idx = self._buf.index(b"\n")
            line = bytes(self._buf[:idx])
            del self._buf[: idx + 1]
            if line:
                out.append(line)
        return out
