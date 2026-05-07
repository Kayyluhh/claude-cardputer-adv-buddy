"""Tail a CC session JSONL for usage stats."""
from __future__ import annotations

import json
import logging
from dataclasses import dataclass
from pathlib import Path

log = logging.getLogger(__name__)


@dataclass
class HarvestResult:
    tokens: int
    new_offset: int


def harvest_usage(path: Path, *, offset: int) -> HarvestResult:
    """Read assistant.message.usage.output_tokens from new JSONL records since offset.

    Returns total tokens added and the new offset. If the last line is incomplete (no
    trailing newline), its bytes are not consumed so the next call retries.
    """
    if not path.exists():
        return HarvestResult(tokens=0, new_offset=0)
    size = path.stat().st_size
    if offset > size:
        # File truncated/rotated since last read. Restart from the beginning.
        log.info("transcript: file %s shrank (was %d, now %d); restarting from offset 0", path, offset, size)
        offset = 0
    if offset >= size:
        return HarvestResult(tokens=0, new_offset=offset)

    with open(path, "rb") as f:
        f.seek(offset)
        chunk = f.read()

    # Split on \n. If the chunk does not end with \n, the last fragment is partial.
    has_partial_tail = not chunk.endswith(b"\n")
    lines = chunk.split(b"\n")
    if has_partial_tail:
        partial = lines[-1]
        lines = lines[:-1]
    else:
        partial = b""

    consumed = len(chunk) - len(partial)
    tokens = 0
    for raw in lines:
        if not raw.strip():
            continue
        try:
            rec = json.loads(raw)
        except json.JSONDecodeError:
            log.debug("transcript: skipping unparseable line at offset %d", offset)
            continue
        tokens += _extract_output_tokens(rec)

    return HarvestResult(tokens=tokens, new_offset=offset + consumed)


def _extract_output_tokens(record) -> int:
    """Defensive extraction. Schema may shift; tolerate missing fields."""
    if not isinstance(record, dict):
        return 0
    if record.get("type") != "assistant":
        return 0
    msg = record.get("message")
    if not isinstance(msg, dict):
        return 0
    usage = msg.get("usage")
    if not isinstance(usage, dict):
        return 0
    val = usage.get("output_tokens")
    return int(val) if isinstance(val, (int, float)) else 0
