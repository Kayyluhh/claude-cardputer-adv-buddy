"""Asyncio main loop wiring hook server + BLE client + state."""
from __future__ import annotations

import asyncio
import base64
import json
import logging
import time
from datetime import date
from pathlib import Path
from typing import Callable

from . import persistence, state as state_mod, transcript, wire
from .ble_client import BleClient, _default_factory
from .hook_server import HookServer

log = logging.getLogger(__name__)

HEARTBEAT_KEEPALIVE_S = 10.0
REAPER_INTERVAL_S = 30.0
DEFAULT_PERMISSION_TIMEOUT_S = 5.0


class Daemon:
    def __init__(
        self,
        state_dir: Path,
        sock_path: Path,
        ble_factory: Callable[[str], object] = _default_factory,
    ) -> None:
        self.state_dir = state_dir
        self.sock_path = sock_path
        self._ble_factory = ble_factory
        self.state = state_mod.GlobalState()
        self.config = persistence.load_config(state_dir / "config.json")
        self.persisted = persistence.load_state(state_dir / "state.json")
        self.state.tokens_today = self.persisted.tokens_today
        self.state.tokens_today_date = (
            date.fromisoformat(self.persisted.tokens_today_date)
            if self.persisted.tokens_today_date
            else None
        )
        self.state.muted_sessions = persistence.load_muted_sessions(
            state_dir / "muted-sessions.json"
        )
        self.permission_timeout_s = self.config.permission_timeout_ms / 1000.0
        self.session_ttl_s = self.config.device_idle_timeout_ms / 1000.0
        self.shutdown_event = asyncio.Event()

        self._hook_server = HookServer(sock_path, self._on_hook)
        self._ble: BleClient | None = None
        self._last_snapshot_repr: str | None = None
        self._last_heartbeat_at = 0.0
        self._push_ack_queue: asyncio.Queue | None = None

    # ---- lifecycle ----

    async def run(self) -> None:
        await self._hook_server.start()
        if self.config.device_address:
            self._ble = BleClient(
                address=self.config.device_address,
                on_message=self._on_ble_message,
                client_factory=self._ble_factory,
            )
            try:
                await self._ble.connect()
                self.state.ble_connected = True
                self.state.device_name = self.config.device_name
                self.state.owner_name = self.config.owner_name
                await self._send_one_shots()
            except Exception:
                log.exception("BLE connect failed; daemon continues without device")
                self.state.ble_connected = False

        try:
            await asyncio.gather(
                self._heartbeat_loop(),
                self._reaper_loop(),
                self._wait_shutdown(),
            )
        finally:
            await self._hook_server.stop()
            if self._ble is not None:
                await self._ble.disconnect()

    async def _wait_shutdown(self) -> None:
        await self.shutdown_event.wait()

    # ---- hook dispatch ----

    async def _on_hook(self, msg: dict, respond) -> None:
        op = msg.get("op")
        if op == "prehook":
            decision = await self._handle_prehook(msg)
            await respond({"decision": decision, "reason": "hardware buddy"})
            return
        if op == "event":
            await self._handle_event(msg)
            return
        if op == "status":
            await respond(self._snapshot_for_status())
            return
        if op == "mute":
            sid = msg.get("session_id", "")
            if sid:
                self.state.muted_sessions.add(sid)
                self._persist_mute()
            await respond({"ok": True})
            return
        if op == "unmute":
            self.state.muted_sessions.discard(msg.get("session_id", ""))
            self._persist_mute()
            await respond({"ok": True})
            return
        if op == "current_session":
            cwd = msg.get("cwd", "")
            sid = self._pick_current_session(cwd)
            await respond({"session_id": sid})
            return
        if op == "prehook_timeout":
            tid = msg.get("tool_use_id", "")
            self._resolve_pending(tid, "ask")
            return
        if op == "push":
            await self._handle_push(msg, respond)
            return
        log.warning("daemon: unknown op %r", op)

    async def _handle_prehook(self, msg: dict) -> str:
        sid = msg.get("session_id", "")
        tid = msg.get("tool_use_id", "")
        sess = self._touch_session(sid, msg.get("transcript_path", ""), msg.get("cwd", ""))
        if sid in self.state.muted_sessions:
            return "ask"
        tool_name = msg.get("tool_name", "")
        hint = state_mod._hint_for(tool_name, msg.get("tool_input", {}))
        loop = asyncio.get_running_loop()
        future: asyncio.Future = loop.create_future()
        prompt = state_mod.PendingPrompt(
            tool_use_id=tid, tool_name=tool_name, hint=hint, future=future,
            arrived_at=time.time(),
        )
        sess.pending_prompt = prompt
        sess.state = "waiting"
        self.state.pending_by_id[tid] = prompt
        self.state.last_msg = state_mod.derive_msg(
            "PreToolUse", msg, awaiting_permission=True
        ) or self.state.last_msg
        state_mod.append_entry(self.state, self.state.last_msg)
        await self._send_heartbeat(force=True)
        try:
            decision = await asyncio.wait_for(future, timeout=self.permission_timeout_s)
            return decision
        except asyncio.TimeoutError:
            return "ask"
        finally:
            sess.pending_prompt = None
            sess.state = "running"
            self.state.pending_by_id.pop(tid, None)
            await self._send_heartbeat(force=True)

    async def _handle_event(self, msg: dict) -> None:
        sid = msg.get("session_id", "")
        event = msg.get("event", "")
        sess = self._touch_session(sid, msg.get("transcript_path", ""), msg.get("cwd", ""))

        if event == "SessionStart":
            sess.state = "idle"
        elif event == "SessionEnd":
            self.state.sessions.pop(sid, None)
        elif event == "Stop":
            sess.state = "idle"
            self._harvest_tokens(sess)
        elif event == "PostToolUse":
            sess.state = "running"

        if sid in self.state.muted_sessions:
            return

        derived = state_mod.derive_msg(event, {
            **msg,
            "session_count": len(self.state.sessions),
        })
        if derived:
            self.state.last_msg = derived
            state_mod.append_entry(self.state, derived)
        await self._send_heartbeat()

    def _touch_session(self, sid: str, transcript_path: str, cwd: str) -> state_mod.Session:
        now = time.time()
        sess = self.state.sessions.get(sid)
        if sess is None:
            sess = state_mod.Session(
                id=sid, started_at=now, last_activity=now,
                state="idle", transcript_path=transcript_path, cwd=cwd,
            )
            self.state.sessions[sid] = sess
        sess.last_activity = now
        if transcript_path and not sess.transcript_path:
            sess.transcript_path = transcript_path
        if cwd and not sess.cwd:
            sess.cwd = cwd
        return sess

    def _pick_current_session(self, cwd: str) -> str | None:
        candidates = list(self.state.sessions.values())
        if cwd:
            cwd_match = [s for s in candidates if s.cwd == cwd]
            if cwd_match:
                cwd_match.sort(key=lambda s: s.last_activity, reverse=True)
                return cwd_match[0].id
        if not candidates:
            return None
        candidates.sort(key=lambda s: s.last_activity, reverse=True)
        return candidates[0].id

    def _resolve_pending(self, tool_use_id: str, decision: str) -> None:
        prompt = self.state.pending_by_id.get(tool_use_id)
        if prompt is None or prompt.future.done():
            return
        prompt.future.set_result(decision)

    # ---- BLE side ----

    def _on_ble_message(self, msg: dict) -> None:
        if msg.get("cmd") == "permission":
            decision = "allow" if msg.get("decision") == "once" else "deny"
            self._resolve_pending(msg.get("id", ""), decision)
            return
        # Route ack messages to in-flight push, if any.
        if "ack" in msg and self._push_ack_queue is not None:
            try:
                self._push_ack_queue.put_nowait(msg)
            except Exception:
                pass

    async def _send_one_shots(self) -> None:
        if self._ble is None:
            return
        await self._ble.send({"time": [int(time.time()), -time.timezone]})
        if self.config.owner_name:
            await self._ble.send({"cmd": "owner", "name": self.config.owner_name})

    async def _heartbeat_loop(self) -> None:
        while not self.shutdown_event.is_set():
            try:
                # If shutdown_event resolves within timeout, break promptly.
                await asyncio.wait_for(self.shutdown_event.wait(), timeout=HEARTBEAT_KEEPALIVE_S)
                break
            except asyncio.TimeoutError:
                pass
            await self._send_heartbeat(force=True)

    async def _reaper_loop(self) -> None:
        while not self.shutdown_event.is_set():
            try:
                await asyncio.wait_for(self.shutdown_event.wait(), timeout=REAPER_INTERVAL_S)
                break
            except asyncio.TimeoutError:
                pass
            state_mod.reap_stale_sessions(
                self.state, now=time.time(), ttl_seconds=self.session_ttl_s
            )

    async def _send_heartbeat(self, *, force: bool = False) -> None:
        state_mod.maybe_rollover_tokens(self.state, today=date.today())
        snapshot = self._build_snapshot()
        rep = repr(sorted(snapshot.items()))
        now = time.time()
        if not force and rep == self._last_snapshot_repr and now - self._last_heartbeat_at < HEARTBEAT_KEEPALIVE_S:
            return
        self._last_snapshot_repr = rep
        self._last_heartbeat_at = now
        if self._ble is not None and self.state.ble_connected:
            try:
                await self._ble.send(snapshot)
            except Exception:
                log.exception("daemon: heartbeat send failed")
        self._persist_state()

    def _build_snapshot(self) -> dict:
        running = sum(1 for s in self.state.sessions.values() if s.state == "running")
        waiting = sum(1 for s in self.state.sessions.values() if s.state == "waiting")
        out = {
            "total": len(self.state.sessions),
            "running": running,
            "waiting": waiting,
            "msg": self.state.last_msg,
            "entries": state_mod.wire_entries(self.state),
            "tokens": self.state.tokens_cumulative,
            "tokens_today": self.state.tokens_today,
        }
        # Most recently arrived pending prompt.
        if self.state.pending_by_id:
            newest = max(self.state.pending_by_id.values(), key=lambda p: p.arrived_at)
            out["prompt"] = {"id": newest.tool_use_id, "tool": newest.tool_name, "hint": newest.hint}
        return out

    def _snapshot_for_status(self) -> dict:
        return {
            "ok": True,
            "data": {
                "ble": "connected" if self.state.ble_connected else "disconnected",
                "device": self.config.device_name,
                "sessions": len(self.state.sessions),
                "muted_sessions": sorted(self.state.muted_sessions),
                **{k: v for k, v in self._build_snapshot().items() if k != "prompt"},
            },
        }

    def _harvest_tokens(self, sess: state_mod.Session) -> None:
        if not sess.transcript_path:
            return
        result = transcript.harvest_usage(Path(sess.transcript_path), offset=sess.transcript_offset)
        sess.transcript_offset = result.new_offset
        if result.tokens > 0:
            self.state.tokens_cumulative += result.tokens
            self.state.tokens_today += result.tokens
            self._persist_state()

    def _persist_state(self) -> None:
        d = self.state.tokens_today_date
        persistence.save_state(
            self.state_dir / "state.json",
            persistence.PersistedState(
                tokens_today=self.state.tokens_today,
                tokens_today_date=d.isoformat() if d else "",
                tokens_lifetime=self.state.tokens_cumulative,
            ),
        )

    def _persist_mute(self) -> None:
        persistence.save_muted_sessions(
            self.state_dir / "muted-sessions.json",
            self.state.muted_sessions,
        )

    # ---- folder push ----

    async def _handle_push(self, msg: dict, respond) -> None:
        path = Path(msg.get("path", ""))
        if not path.is_dir():
            await respond({"stage": "error", "msg": f"not a directory: {path}"})
            return

        # Enumerate regular files (no recursion, dotfiles skipped).
        files: list[Path] = sorted(
            p for p in path.iterdir()
            if p.is_file() and not p.name.startswith(".")
        )
        total = sum(p.stat().st_size for p in files)
        if total > 1_800_000:
            await respond({"stage": "error",
                           "msg": f"folder size {total} bytes exceeds 1.8 MB cap"})
            return

        # Determine pack name.
        pack_name = path.name
        manifest = path / "manifest.json"
        if manifest.exists():
            try:
                pack_name = json.loads(manifest.read_text()).get("name", pack_name)
            except json.JSONDecodeError:
                pass

        if self._ble is None or not self.state.ble_connected:
            await respond({"stage": "error", "msg": "device not connected"})
            return

        if self._push_ack_queue is not None:
            await respond({"stage": "error", "msg": "push already in progress"})
            return

        # Acks come asynchronously through _on_ble_message; route them via a queue here.
        ack_q: asyncio.Queue[dict] = asyncio.Queue()
        self._push_ack_queue = ack_q

        async def wait_ack(name: str, timeout: float = 5.0) -> dict:
            while True:
                m = await asyncio.wait_for(ack_q.get(), timeout=timeout)
                if m.get("ack") == name:
                    return m

        try:
            await self._ble.send({"cmd": "char_begin", "name": pack_name, "total": total})
            ack = await wait_ack("char_begin", timeout=3.0)
            if not ack.get("ok"):
                await respond({"stage": "error", "msg": "device declined push"})
                return
            await respond({"stage": "begin", "name": pack_name, "total": total})

            for f in files:
                size = f.stat().st_size
                await self._ble.send({"cmd": "file", "path": f.name, "size": size})
                ack = await wait_ack("file")
                if not ack.get("ok"):
                    await respond({"stage": "error", "msg": f"device rejected file {f.name}"})
                    return
                await respond({"stage": "file", "name": f.name, "size": size})

                with open(f, "rb") as fh:
                    while True:
                        chunk = fh.read(120)  # 120 raw bytes = 160 base64 chars; envelope total ≈182 bytes ≈ MTU-3 on macOS (185)
                        if not chunk:
                            break
                        b64 = base64.b64encode(chunk).decode("ascii")
                        await self._ble.send({"cmd": "chunk", "d": b64})
                        ack = await wait_ack("chunk")
                        await respond({"stage": "chunk", "n": ack.get("n", 0)})

                await self._ble.send({"cmd": "file_end"})
                ack = await wait_ack("file_end")
                if not ack.get("ok"):
                    await respond({"stage": "error", "msg": f"file {f.name} write incomplete"})
                    return
                await respond({"stage": "file_end", "size": ack.get("n", 0)})

            await self._ble.send({"cmd": "char_end"})
            ack = await wait_ack("char_end")
            if not ack.get("ok"):
                await respond({"stage": "error", "msg": "device char_end failed"})
                return
            await respond({"stage": "done"})
        finally:
            self._push_ack_queue = None


def main() -> int:
    """Entrypoint when run as `python -m claude_buddy.daemon`. Used by run.py."""
    import os
    if env_dir := os.environ.get("BUDDY_STATE_DIR"):
        state_dir = Path(env_dir)
    else:
        home = Path(os.environ.get("HOME", "~")).expanduser()
        state_dir = home / ".claude-buddy"
    state_dir.mkdir(parents=True, exist_ok=True)
    sock_path = Path("/tmp/claude-buddy.sock")
    logging.basicConfig(
        level=logging.INFO,
        filename=str(state_dir / "daemon.log"),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    d = Daemon(state_dir=state_dir, sock_path=sock_path)
    asyncio.run(d.run())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
