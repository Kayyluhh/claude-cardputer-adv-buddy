"""bleak-based NUS client. Real BLE traffic for the Cardputer."""
from __future__ import annotations

import asyncio
import json
import logging
from typing import Callable, Protocol

from bleak import BleakClient as _RealBleakClient

from .wire import LineBuffer, NUS_RX_UUID, NUS_TX_UUID, encode_line

log = logging.getLogger(__name__)


class _BleakLike(Protocol):
    """The subset of bleak.BleakClient we depend on. Lets tests inject stubs."""
    is_connected: bool
    async def connect(self) -> bool: ...
    async def disconnect(self) -> bool: ...
    async def pair(self) -> bool: ...
    async def start_notify(self, char_uuid: str, callback) -> None: ...
    async def stop_notify(self, char_uuid: str) -> None: ...
    async def write_gatt_char(self, char_uuid: str, data: bytes, response: bool = False) -> None: ...


def _default_factory(address: str) -> _BleakLike:
    return _RealBleakClient(address)


class BleClient:
    """High-level NUS client. send(dict) writes a line; on_message(dict) fires per line."""

    def __init__(
        self,
        address: str,
        on_message: Callable[[dict], None],
        client_factory: Callable[[str], _BleakLike] = _default_factory,
    ) -> None:
        self._address = address
        self._on_message = on_message
        self._factory = client_factory
        self._client: _BleakLike | None = None
        self._line_buffer = LineBuffer()

    @property
    def is_connected(self) -> bool:
        return self._client is not None and self._client.is_connected

    async def connect(self) -> None:
        client = self._factory(self._address)
        await client.connect()
        try:
            await client.pair()
        except Exception:
            log.debug("pair() not supported or already paired")
        await client.start_notify(NUS_TX_UUID, self._on_notify)
        self._client = client
        log.info("BLE connected to %s", self._address)

    async def disconnect(self) -> None:
        if self._client is None:
            return
        try:
            await self._client.stop_notify(NUS_TX_UUID)
        except Exception:
            pass
        try:
            await self._client.disconnect()
        finally:
            self._client = None

    async def send(self, payload: dict) -> None:
        if self._client is None:
            raise RuntimeError("ble_client: not connected")
        data = encode_line(payload)
        await self._client.write_gatt_char(NUS_RX_UUID, data, response=False)

    def _on_notify(self, _handle, data: bytearray) -> None:
        for raw in self._line_buffer.feed(bytes(data)):
            try:
                msg = json.loads(raw)
            except json.JSONDecodeError:
                log.warning("ble_client: dropping unparseable notification: %r", raw)
                continue
            try:
                self._on_message(msg)
            except Exception:
                log.exception("ble_client: on_message callback raised")
