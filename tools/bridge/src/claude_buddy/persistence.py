"""Atomic JSON persistence for ~/.claude-buddy/{state,config,muted-sessions}.json."""
from __future__ import annotations

import json
import logging
import os
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

log = logging.getLogger(__name__)


def _coerce(raw: Any, key: str, default: Any) -> Any:
    """Return raw[key] if its type matches default's, else default. Logs a warning on mismatch."""
    if not isinstance(raw, dict):
        return default
    val = raw.get(key, default)
    if default is None:
        # str | None field — accept str or None.
        if val is None or isinstance(val, str):
            return val
        log.warning("persistence: %s wrong type %s, using None", key, type(val).__name__)
        return None
    if isinstance(val, type(default)) and not isinstance(val, bool):
        # bool is an int subclass in Python; reject it for int fields.
        return val
    if isinstance(default, int) and isinstance(val, bool):
        log.warning("persistence: %s got bool, using default", key)
        return default
    if not isinstance(val, type(default)):
        log.warning("persistence: %s wrong type %s, using default", key, type(val).__name__)
        return default
    return val


def atomic_write_json(path: Path, data: Any) -> None:
    """Write JSON to path atomically: tmp + fsync + rename. POSIX-atomic."""
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


@dataclass
class PersistedState:
    tokens_today: int = 0
    tokens_today_date: str = ""  # YYYY-MM-DD
    tokens_lifetime: int = 0


def load_state(path: Path) -> PersistedState:
    if not path.exists():
        return PersistedState()
    try:
        raw = json.loads(path.read_text())
    except (json.JSONDecodeError, ValueError, AttributeError, OSError) as exc:
        log.warning("persistence: could not load state from %s: %s", path, exc, exc_info=False)
        return PersistedState()
    if not isinstance(raw, dict):
        log.warning("persistence: state file %s has wrong root type %s, using defaults", path, type(raw).__name__)
        return PersistedState()
    return PersistedState(
        tokens_today=_coerce(raw, "tokens_today", 0),
        tokens_today_date=_coerce(raw, "tokens_today_date", ""),
        tokens_lifetime=_coerce(raw, "tokens_lifetime", 0),
    )


def save_state(path: Path, state: PersistedState) -> None:
    atomic_write_json(path, asdict(state))


@dataclass
class Config:
    device_address: str | None = None
    device_name: str | None = None
    owner_name: str | None = None
    permission_timeout_ms: int = 30000
    device_idle_timeout_ms: int = 600000  # 10 minutes


def load_config(path: Path) -> Config:
    if not path.exists():
        return Config()
    try:
        raw = json.loads(path.read_text())
    except (json.JSONDecodeError, ValueError, AttributeError, OSError) as exc:
        log.warning("persistence: could not load config from %s: %s", path, exc, exc_info=False)
        return Config()
    if not isinstance(raw, dict):
        log.warning("persistence: config file %s has wrong root type %s, using defaults", path, type(raw).__name__)
        return Config()
    return Config(
        device_address=_coerce(raw, "device_address", None),
        device_name=_coerce(raw, "device_name", None),
        owner_name=_coerce(raw, "owner_name", None),
        permission_timeout_ms=_coerce(raw, "permission_timeout_ms", 30000),
        device_idle_timeout_ms=_coerce(raw, "device_idle_timeout_ms", 600000),
    )


def save_config(path: Path, cfg: Config) -> None:
    atomic_write_json(path, asdict(cfg))


def load_muted_sessions(path: Path) -> set[str]:
    if not path.exists():
        return set()
    try:
        raw = json.loads(path.read_text())
    except (json.JSONDecodeError, ValueError, AttributeError, OSError) as exc:
        log.warning("persistence: could not load muted sessions from %s: %s", path, exc, exc_info=False)
        return set()
    if isinstance(raw, dict):
        items = raw.keys()
    elif isinstance(raw, list):
        items = raw
    else:
        log.warning("persistence: muted sessions file %s has wrong root type %s, using defaults", path, type(raw).__name__)
        return set()
    result: set[str] = set()
    for item in items:
        if isinstance(item, str):
            result.add(item)
        else:
            log.debug("persistence: muted sessions skipping non-string item %r", item)
    return result


def save_muted_sessions(path: Path, sessions: set[str]) -> None:
    atomic_write_json(path, sorted(sessions))
