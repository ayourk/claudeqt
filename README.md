# ClaudeQt

Native Qt6 desktop GUI for the [Claude Code CLI](https://docs.anthropic.com/claude/claude-code),
with an optional headless remote daemon for cross-host execution.

> **Phase 1 status:** scaffold only. The GUI is a `QLabel` stub and the
> remote daemon is a signalfd+timerfd+sd_notify watchdog loop. The real
> chat UI, JSON-RPC engine, and TLS transport arrive in Phases 2–6.
> See [`docs/phase-1.md`](docs/phase-1.md) for the full plan.

## Installation (Ubuntu Noble)

```sh
sudo add-apt-repository ppa:ayourk/claudeqt
sudo apt update
sudo apt install claudeqt          # GUI only
sudo apt install claudeqt-remote   # Headless daemon
```

The GUI requires the Claude Code CLI to be installed separately —
Anthropic does not ship a Debian package. On first launch ClaudeQt
offers an install dialog with copy-pasteable commands.

## Building from source

```sh
sudo apt build-dep .
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

Depends on Qt 6.4.2+, KF5 syntax-highlighting (Qt6 backport from the
same PPA) or KF6 syntax-highlighting, libcmark-gfm, SQLite 3.45+,
Python 3.12+, and libsystemd on Linux.

## License

GPL-3.0-only. See [LICENSE](LICENSE).
