# ClaudeQt 0.0.1 (Phase 1 scaffold)

First source upload to `ppa:ayourk/claudeqt`. This release is a
**scaffold only** — the GUI is a single QLabel and the remote daemon
is a signalfd+timerfd watchdog loop. Its purpose is to validate the
full packaging, systemd, and Launchpad build path end to end before
Phase 2 opens.

## What's in the package

- `claudeqt` — QLabel GUI stub linking Qt6, KF5-qt6 syntax-highlighting,
  libcmark-gfm, SQLite3, and embedded Python3. Proves the CMake graph
  and dependency resolution work under Launchpad's Noble chroot.
- `claudeqt-remote` — Type=notify daemon that calls `sd_notify(READY=1)`
  then pings `WATCHDOG=1` every 10 s against the unit's `WatchdogSec=30`
  ceiling. Exercises the systemd liveness path with a real process.
- `claudeqt-common` — arch:all reservation for Phase 2+ resources
  (icons, desktop entry, translations, Python engine bridge).

## What's not in the package

Everything else — chat view, Markdown renderer, SQLite schema,
JSON-RPC protocol, TLS transport, MCP integration, slash commands,
settings dialog, first-run CLI install dialog. All arrive in later
phases. See `docs/phase-1.md` §10 for the phase breakdown.

## Installation

```sh
sudo add-apt-repository ppa:ayourk/claudeqt
sudo apt install claudeqt claudeqt-remote
```

Noble only in Phase 1. Resolute upload follows when the port is
validated.
