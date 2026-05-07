"""Shared pytest fixtures for the claude_buddy test suite."""
from __future__ import annotations

import pytest


@pytest.fixture
def tmp_state_dir(tmp_path, monkeypatch):
    """Redirect ~/.claude-buddy to a tmp_path. Use in tests that touch persistence."""
    home = tmp_path / "home"
    (home / ".claude-buddy").mkdir(parents=True)
    monkeypatch.setenv("HOME", str(home))
    return home / ".claude-buddy"
