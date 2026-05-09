---
name: buddy-gifpush
description: This skill should be used when the user asks to "push to my buddy", "send a character to the cardputer", "push folder to buddy", "send gif pack", "push character pack", or types /buddy-gifpush with a folder path. Streams a folder over BLE to the Hardware Buddy device via the running bridge daemon.
---

# Push a folder to the Hardware Buddy device

The bundled push client connects to the running daemon over `/tmp/claude-buddy.sock`; the daemon drives the BLE folder-push transport (`char_begin → file/chunk*/file_end → char_end`).

## When invoked

1. Resolve the folder argument from the user's message to an absolute path. Reject if missing or not a directory.

2. Sum the folder's regular-file sizes. Reject if total exceeds 1.8 MB — the device firmware will refuse it.

3. Check `/tmp/claude-buddy.sock` exists. If not, tell the user to run `/buddy-run` first and stop.

4. Run:

   ```bash
   ~/.claude-buddy/python -m claude_buddy.push "<absolute-path>"
   ```

5. The client streams JSON progress lines: `{stage: "begin"}`, `{stage: "file", name, size}`, `{stage: "chunk", n}`, `{stage: "file_end", size}`, `{stage: "done"}` or `{stage: "error", msg}`.

6. Summarize for the user: pack name, count of files, total bytes, success/failure. On error, also suggest `tail -n 50 ~/.claude-buddy/daemon.log`.

## Notes

- A `manifest.json` with a `name` field overrides the folder name on the device.
- Dotfiles are skipped automatically.
- The push is strictly sequential (one chunk at a time); a 1.5 MB pack typically takes ~30 seconds.
