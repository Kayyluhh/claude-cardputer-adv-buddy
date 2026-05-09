"""Background-spawn the daemon. Used by the buddy-run skill."""
from __future__ import annotations

import argparse
import json
import os
import signal
import sys
from pathlib import Path


def _is_pid_alive(pid: int) -> bool:
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def _read_pid(path: Path) -> int | None:
    if not path.exists():
        return None
    try:
        return int(path.read_text().strip())
    except (OSError, ValueError):
        return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--state-dir", default=str(Path.home() / ".claude-buddy"))
    args = parser.parse_args()

    state_dir = Path(args.state_dir)
    state_dir.mkdir(parents=True, exist_ok=True)
    pid_file = state_dir / "daemon.pid"
    log_file = state_dir / "daemon.log"

    existing = _read_pid(pid_file)
    if existing is not None and _is_pid_alive(existing):
        print(json.dumps({"ok": False, "error": "already_running", "pid": existing}))
        return 1
    if existing is not None:
        pid_file.unlink(missing_ok=True)

    # First fork.
    if os.fork() != 0:
        # Parent exits immediately.
        print(json.dumps({"ok": True, "status": "spawning"}))
        return 0
    os.setsid()
    if os.fork() != 0:
        # Intermediate exits.
        os._exit(0)

    # Grandchild: redirect stdio.
    sys.stdout.flush()
    sys.stderr.flush()
    log_fd = os.open(str(log_file), os.O_APPEND | os.O_CREAT | os.O_WRONLY, 0o644)
    os.dup2(log_fd, 1)
    os.dup2(log_fd, 2)
    os.close(log_fd)
    devnull = os.open(os.devnull, os.O_RDONLY)
    os.dup2(devnull, 0)
    os.close(devnull)

    # Install handlers before writing the PID file so a SIGTERM in the
    # micro-window can't leave a stale PID behind.
    def _cleanup(*_):
        try:
            pid_file.unlink(missing_ok=True)
        except Exception:
            pass
        os._exit(0)

    signal.signal(signal.SIGTERM, _cleanup)
    signal.signal(signal.SIGINT, _cleanup)

    pid_file.write_text(str(os.getpid()))

    # Hand off to daemon.main(); never returns under normal operation.
    os.environ["BUDDY_STATE_DIR"] = str(state_dir)
    from . import daemon as daemon_mod
    raise SystemExit(daemon_mod.main())


if __name__ == "__main__":
    raise SystemExit(main())
