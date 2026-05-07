from __future__ import annotations

import asyncio
import json

import pytest

from .ble_fake import BleFake


class TestBleFake:
    async def test_send_to_device_via_rx(self):
        fake = BleFake()
        await fake.write_rx(b'{"hello":1}\n')
        line = await asyncio.wait_for(fake.received_from_host(), timeout=1.0)
        assert line == {"hello": 1}

    async def test_device_notifies_via_tx(self):
        fake = BleFake()
        notifications: list[dict] = []

        def on_notify(data: bytes) -> None:
            for chunk in data.split(b"\n"):
                if chunk:
                    notifications.append(json.loads(chunk))

        fake.set_notify_callback(on_notify)
        await fake.notify({"ack": "status", "ok": True})
        # Allow the queue to drain.
        await asyncio.sleep(0.01)
        assert notifications == [{"ack": "status", "ok": True}]

    async def test_status_response_canned(self):
        fake = BleFake()
        # When fake receives a status command, it auto-replies with a canned status.
        notifications: list[dict] = []
        fake.set_notify_callback(lambda d: notifications.append(json.loads(d.strip())))
        await fake.write_rx(b'{"cmd":"status"}\n')
        await asyncio.sleep(0.05)
        assert any(n.get("ack") == "status" for n in notifications)
