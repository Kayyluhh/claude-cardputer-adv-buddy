from __future__ import annotations

import json

import pytest

from claude_buddy import wire


class TestUUIDs:
    def test_nus_service_uuid(self):
        assert wire.NUS_SERVICE_UUID == "6e400001-b5a3-f393-e0a9-e50e24dcca9e"

    def test_rx_uuid_desktop_to_device(self):
        assert wire.NUS_RX_UUID == "6e400002-b5a3-f393-e0a9-e50e24dcca9e"

    def test_tx_uuid_device_to_desktop(self):
        assert wire.NUS_TX_UUID == "6e400003-b5a3-f393-e0a9-e50e24dcca9e"


class TestEncodeLine:
    def test_appends_newline(self):
        out = wire.encode_line({"a": 1})
        assert out.endswith(b"\n")

    def test_utf8_encoding(self):
        out = wire.encode_line({"msg": "café"})
        assert out == '{"msg":"café"}\n'.encode("utf-8")

    def test_compact_separators(self):
        out = wire.encode_line({"a": 1, "b": 2})
        assert b" " not in out.replace(b'"a"', b"").replace(b'"b"', b"")  # no extra spaces


class TestLineBuffer:
    def test_collects_until_newline(self):
        buf = wire.LineBuffer()
        out = buf.feed(b'{"a":1}\n{"b":2}\n')
        assert [json.loads(line) for line in out] == [{"a": 1}, {"b": 2}]

    def test_partial_line_held(self):
        buf = wire.LineBuffer()
        assert buf.feed(b'{"a":') == []
        assert buf.feed(b'1}\n') == [b'{"a":1}']

    def test_skips_empty_lines(self):
        buf = wire.LineBuffer()
        out = buf.feed(b"\n\n{}\n")
        assert out == [b"{}"]


class TestTurnEventCap:
    def test_under_cap_passes(self):
        evt = {"evt": "turn", "role": "assistant", "content": [{"type": "text", "text": "hi"}]}
        assert wire.turn_event_too_large(evt) is False

    def test_over_cap_drops(self):
        big_text = "x" * 4096
        evt = {"evt": "turn", "role": "assistant", "content": [{"type": "text", "text": big_text}]}
        assert wire.turn_event_too_large(evt) is True

    def test_exact_4kb_boundary(self):
        # Build an event whose UTF-8 serialization is exactly 4096 bytes.
        base = {"evt": "turn", "role": "assistant", "content": [{"type": "text", "text": ""}]}
        baseline = len(wire.encode_line(base)) - 1  # subtract trailing newline
        pad = 4096 - baseline
        evt = {"evt": "turn", "role": "assistant", "content": [{"type": "text", "text": "x" * pad}]}
        assert wire.turn_event_too_large(evt) is False  # exactly 4096 is allowed

        evt2 = {"evt": "turn", "role": "assistant", "content": [{"type": "text", "text": "x" * (pad + 1)}]}
        assert wire.turn_event_too_large(evt2) is True
