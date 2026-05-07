from __future__ import annotations

import json
from pathlib import Path

import pytest

from claude_buddy import transcript


def _write_jsonl(path: Path, records: list[dict]) -> None:
    path.write_text("\n".join(json.dumps(r) for r in records) + "\n")


class TestHarvestUsage:
    def test_nonexistent_file(self, tmp_path):
        result = transcript.harvest_usage(tmp_path / "nope.jsonl", offset=0)
        assert result.tokens == 0
        assert result.new_offset == 0

    def test_empty_file(self, tmp_path):
        path = tmp_path / "t.jsonl"
        path.write_text("")
        result = transcript.harvest_usage(path, offset=0)
        assert result.tokens == 0
        assert result.new_offset == 0

    def test_single_assistant_message(self, tmp_path):
        path = tmp_path / "t.jsonl"
        _write_jsonl(path, [
            {"type": "user", "content": "hi"},
            {"type": "assistant", "message": {"usage": {"output_tokens": 42}}},
        ])
        result = transcript.harvest_usage(path, offset=0)
        assert result.tokens == 42
        assert result.new_offset == path.stat().st_size

    def test_resumes_from_offset(self, tmp_path):
        path = tmp_path / "t.jsonl"
        _write_jsonl(path, [
            {"type": "assistant", "message": {"usage": {"output_tokens": 10}}},
        ])
        first = transcript.harvest_usage(path, offset=0)
        # Append more.
        with open(path, "a") as f:
            f.write(json.dumps({"type": "assistant", "message": {"usage": {"output_tokens": 33}}}) + "\n")
        second = transcript.harvest_usage(path, offset=first.new_offset)
        assert second.tokens == 33

    def test_multiple_assistant_messages(self, tmp_path):
        path = tmp_path / "t.jsonl"
        _write_jsonl(path, [
            {"type": "assistant", "message": {"usage": {"output_tokens": 5}}},
            {"type": "user", "content": "x"},
            {"type": "assistant", "message": {"usage": {"output_tokens": 7}}},
        ])
        result = transcript.harvest_usage(path, offset=0)
        assert result.tokens == 12

    def test_handles_partial_last_line(self, tmp_path):
        path = tmp_path / "t.jsonl"
        # File with a complete record followed by half a JSON object.
        complete = json.dumps({"type": "assistant", "message": {"usage": {"output_tokens": 4}}}) + "\n"
        partial = '{"type": "assist'
        path.write_text(complete + partial)
        result = transcript.harvest_usage(path, offset=0)
        assert result.tokens == 4
        # Offset advances to the start of the partial line so we re-read it next time.
        assert result.new_offset == len(complete.encode("utf-8"))

    def test_tolerates_missing_usage(self, tmp_path):
        path = tmp_path / "t.jsonl"
        _write_jsonl(path, [
            {"type": "assistant", "message": {}},  # no usage
            {"type": "assistant"},  # no message
        ])
        result = transcript.harvest_usage(path, offset=0)
        assert result.tokens == 0

    def test_tolerates_unparseable_lines(self, tmp_path):
        path = tmp_path / "t.jsonl"
        good = json.dumps({"type": "assistant", "message": {"usage": {"output_tokens": 9}}})
        path.write_text(good + "\nthis is not json\n")
        result = transcript.harvest_usage(path, offset=0)
        assert result.tokens == 9

    def test_file_truncation_between_reads(self, tmp_path):
        path = tmp_path / "t.jsonl"
        # First call: read existing content, advance offset.
        _write_jsonl(path, [
            {"type": "assistant", "message": {"usage": {"output_tokens": 50}}},
        ])
        first = transcript.harvest_usage(path, offset=0)
        assert first.tokens == 50
        old_offset = first.new_offset

        # File gets truncated/replaced with smaller content.
        _write_jsonl(path, [
            {"type": "assistant", "message": {"usage": {"output_tokens": 7}}},
        ])
        # Now stat().st_size < old_offset. Implementation should detect this and restart from 0.
        second = transcript.harvest_usage(path, offset=old_offset)
        assert second.tokens == 7
        assert second.new_offset == path.stat().st_size

    def test_non_dict_json_record_tolerated(self, tmp_path):
        path = tmp_path / "t.jsonl"
        # Mix of non-dict JSON and a valid record.
        path.write_text("null\n42\n[]\n" + json.dumps({"type": "assistant", "message": {"usage": {"output_tokens": 11}}}) + "\n")
        result = transcript.harvest_usage(path, offset=0)
        assert result.tokens == 11
