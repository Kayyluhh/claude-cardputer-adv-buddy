from __future__ import annotations

import asyncio
import json

import pytest

from claude_buddy import ble_client, wire
from .ble_fake import BleFake


class _StubBleakClient:
    """Minimal stub that mimics the bleak.BleakClient surface BleClient uses."""

    def __init__(self, address: str, fake: BleFake) -> None:
        self.address = address
        self.is_connected = False
        self._fake = fake
        self._notify_cb = None
        fake.set_notify_callback(self._dispatch_notify)

    def _dispatch_notify(self, data: bytes) -> None:
        if self._notify_cb:
            self._notify_cb(0, bytearray(data))

    async def connect(self) -> bool:
        self.is_connected = True
        return True

    async def disconnect(self) -> bool:
        self.is_connected = False
        return True

    async def pair(self) -> bool:
        return True

    async def start_notify(self, char_uuid, callback):
        self._notify_cb = callback

    async def stop_notify(self, char_uuid):
        self._notify_cb = None

    async def write_gatt_char(self, char_uuid, data, response=False):
        await self._fake.write_rx(bytes(data))


@pytest.fixture
async def client_pair():
    fake = BleFake()
    received: list[dict] = []

    def on_msg(msg: dict) -> None:
        received.append(msg)

    client = ble_client.BleClient(
        address="AA:BB:CC:DD:EE:FF",
        on_message=on_msg,
        client_factory=lambda addr: _StubBleakClient(addr, fake),
    )
    await client.connect()
    yield client, fake, received
    await client.disconnect()


class TestBleClient:
    async def test_send_writes_to_rx(self, client_pair):
        client, fake, _ = client_pair
        await client.send({"cmd": "status"})
        msg = await asyncio.wait_for(fake.received_from_host(), timeout=1.0)
        assert msg == {"cmd": "status"}

    async def test_notification_parsed_via_callback(self, client_pair):
        client, fake, received = client_pair
        await fake.notify({"foo": 1})
        await asyncio.sleep(0.05)
        assert received == [{"foo": 1}]

    async def test_fragmented_notification_reassembled(self, client_pair):
        client, fake, received = client_pair
        # Simulate MTU fragmentation: bleak callbacks fire per-packet.
        if fake._notify_cb is not None:
            fake._notify_cb(b'{"a":')
            fake._notify_cb(b'1}\n')
        await asyncio.sleep(0.05)
        assert received == [{"a": 1}]

    async def test_status_auto_replies(self, client_pair):
        client, fake, received = client_pair
        await client.send({"cmd": "status"})
        await asyncio.sleep(0.05)
        assert any(m.get("ack") == "status" for m in received)
