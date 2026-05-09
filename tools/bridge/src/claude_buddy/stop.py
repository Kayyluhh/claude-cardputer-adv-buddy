"""Stop the daemon by PID."""
from __future__ import annotations

import argparse
import json
import os
import signal
import time
from pathlib import Path

from .run import _is_pid_alive, _read_pid


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--state-dir", default=str(Path.home() / ".claude-buddy"))
    parser.add_argument("--timeout", type=float, default=5.0)
    args = parser.parse_args()

    state_dir = Path(args.state_dir)
    pid_file = state_dir / "daemon.pid"
    pid = _read_pid(pid_file)

    if pid is None or not _is_pid_alive(pid):
        pid_file.unlink(missing_ok=True)
        print(json.dumps({"ok": True, "status": "not_running"}))
        return 0

    try:
        os.kill(pid, signal.SIGTERM)
    except ProcessLookupError:
        pid_file.unlink(missing_ok=True)
        print(json.dumps({"ok": True, "status": "not_running"}))
        return 0

    deadline = time.time() + args.timeout
    while time.time() < deadline:
        if not _is_pid_alive(pid):
            pid_file.unlink(missing_ok=True)
            print(json.dumps({"ok": True, "status": "stopped", "pid": pid}))
            return 0
        time.sleep(0.1)

    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    pid_file.unlink(missing_ok=True)
    print(json.dumps({"ok": True, "status": "killed", "pid": pid}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
