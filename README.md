# ClaudeQt

<p align="center">
  <img src="resources/icons/app.svg" alt="ClaudeQt Logo" width="128" height="128">
  <br><br>
  <img src="https://img.shields.io/badge/License-GPL_3.0--only-blue.svg" alt="License: GPL 3.0">
  <img src="https://img.shields.io/github/last-commit/ayourk/claudeqt" alt="Last Commit">
  <img src="https://img.shields.io/badge/PPA-ayourk%2Fclaudeqt-purple.svg" alt="PPA">
  <br>
  <a href="https://github.com/ayourk/claudeqt/actions/workflows/linux-appimage.yml"><img src="https://img.shields.io/github/actions/workflow/status/ayourk/claudeqt/linux-appimage.yml?label=Linux" alt="Linux"></a>
  <a href="https://github.com/ayourk/claudeqt/actions/workflows/macos-build.yml"><img src="https://img.shields.io/github/actions/workflow/status/ayourk/claudeqt/macos-build.yml?label=MacOS" alt="MacOS"></a>
  <a href="https://github.com/ayourk/claudeqt/actions/workflows/windows-build-msvc.yml"><img src="https://img.shields.io/github/actions/workflow/status/ayourk/claudeqt/windows-build-msvc.yml?label=Windows" alt="Windows"></a>
  <a href="https://github.com/ayourk/claudeqt-vcpkg/actions/workflows/build-ports.yml"><img src="https://img.shields.io/github/actions/workflow/status/ayourk/claudeqt-vcpkg/build-ports.yml?label=vcpkg%20ports" alt="vcpkg ports"></a>
</p>

**A native Qt6 desktop GUI for the [Claude Code CLI](https://docs.anthropic.com/claude/claude-code), with an optional headless remote daemon for cross-host execution.**

ClaudeQt wraps the Claude Code CLI in a proper desktop application — project/session tree, tabbed code editor with syntax highlighting, chat shell, multi-window support with cross-instance synchronization, and crash-safe persistence — without replacing the CLI itself. Claude Code remains the only thing that executes shell commands; ClaudeQt is the render, display, and metadata layer.

ClaudeQt is accompanied by **claudeqt-remote**, a headless daemon for running Claude Code sessions on remote hosts, connectable from any ClaudeQt GUI instance.

> **AI Attribution:** This project's documentation, architecture decisions, and code are produced with the assistance of Claude AI (Anthropic), with lots of human oversight, editorial direction, and final decision-making by the project author.

[CodePilot](https://github.com/op7418/CodePilot) was used as inspiration for this application. Development on this application was started before Anthropic created their own desktop app.

## Key Principles

- **CLI-first architecture** — Claude Code CLI is the execution engine; ClaudeQt never runs shell commands itself
- **Crash-safe by default** — all user state (geometry, drafts, tree expansion, session data) persists through `SIGKILL` via SQLite WAL (Write-Ahead Logging) write-through
- **Multi-instance aware** — leader-elected IPC (Inter-Process Communication) hub coordinates cross-instance updates via Unix domain sockets; multiple windows share one database without conflicts
- **Retargetable sessions** — sessions support mid-session working directory changes and resume-into-new-cwd, capabilities the CLI alone lacks
- **Linux-first, portable by design** — developed and packaged for Linux as the primary target, but built on Qt6 and portable abstractions (QLocalServer, QStandardPaths, SQLite) so macOS and Windows support is a packaging exercise, not a rewrite
- **Packaging-first** — ships as proper `.deb` packages via PPA, with `.rpm`, Flatpak, AppImage, and Arch builds via CI

## Features

- **Project/session tree** — hierarchical project grouping with drag-and-drop, right-click context menus, and configurable sort order (alphabetical, last-used, or off)
- **Tabbed code editor** — KSyntaxHighlighting (KSH) with 300+ language definitions, dirty-buffer tracking, external modification detection, and file drag-and-drop
- **Chat shell** — message view with auto-scrolling, multi-line input with auto-resize, draft persistence, and large-paste spill to attachments
- **Geometry persistence** — window size/position, splitter ratios, pane visibility, and tree expansion state survive restarts with debounced write-through
- **Dark theme** — full CSS-analogue palette with semantic colors, applied globally via QSS (Qt Style Sheets)
- **Settings dialog** — General, Editor, and About tabs with live-apply and shake/veto validation
- **Multi-instance hub** — leader election with exponential backoff, epoch-based leader identity, and relay jitter for race-condition testing
- **13 automated test suites** — persistence, UI widgets, tree model CRUD (Create/Read/Update/Delete), chat splitter, settings, theming, instance hub (including multi-process IPC)

## What ClaudeQt Is Not

- **Not an IDE.** ClaudeQt's editor is for reviewing and lightly editing files that come up during a Claude session — not for replacing your development environment. Use your preferred editor for serious coding; use ClaudeQt for the AI conversation and the context around it.
- **Not a terminal emulator.** ClaudeQt never runs shell commands. All execution is delegated to the Claude Code CLI, which handles tool use, permissions, and sandboxing on its own terms.
- **Not a Claude API wrapper.** ClaudeQt talks to the Claude Code CLI, not directly to the Anthropic API. It inherits the CLI's authentication, rate limiting, and model selection rather than reimplementing them.

## Technology Stack

| Layer | Library | License |
|---|---|---|
| GUI toolkit | Qt 6.4.2+ | LGPL 3.0 |
| Syntax highlighting | KF5 SyntaxHighlighting (Qt6 backport) | MIT |
| Markdown rendering | cmark-gfm | BSD 2-Clause |
| Database | SQLite 3.38+ (WAL mode) | Public domain |
| Build system | CMake 3.22+ / Ninja | BSD 3-Clause |
| Daemon notify | libsystemd | LGPL 2.1 |

## Installation (Ubuntu Noble)

```sh
sudo add-apt-repository ppa:ayourk/claudeqt
sudo apt update
sudo apt install claudeqt          # GUI only
sudo apt install claudeqt-remote   # Headless daemon
```

The GUI requires the Claude Code CLI to be installed separately —
Anthropic does not ship a Debian package. After installing, see
`/usr/share/doc/claudeqt/README.Debian` for CLI setup instructions.

## Building from Source

```sh
sudo apt build-dep .
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

To run the test suite (optional):

```sh
ctest --test-dir build --output-on-failure
```

### Build Dependencies (Ubuntu Noble)

| Package | Purpose |
|---|---|
| `qt6-base-dev` | Core Qt6 widgets, SQL, network |
| `libkf5syntaxhighlighting-qt6-dev` | KSH Qt6 backport (from `ppa:ayourk/claudeqt`) |
| `libcmark-gfm-dev`, `libcmark-gfm-extensions-dev` | Markdown to HTML conversion |
| `libsqlite3-dev` | Database engine |
| `libsystemd-dev` | `sd_notify` for the remote daemon |
| `cmake`, `ninja-build` | Build system |

## System Requirements

- **OS:** Ubuntu 24.04 LTS (Noble) — primary target; macOS 10.14+ and Windows 10+ also build via CI
- **Runtime:** Qt 6.4.2+, SQLite 3.38+, KF5 SyntaxHighlighting (Qt6)
- **Optional:** Claude Code CLI (for actual AI interaction; the GUI shell works without it)

## Documentation

- [User Guide](docs/user-guide.md) — keyboard shortcuts, settings, UI behavior
- [Release Notes](docs/RELEASE_NOTES.md) — roadmap and per-version "what changed"

## License

GPL-3.0-only. See [LICENSE](LICENSE).

## Contributing

All commits must include a DCO (Developer Certificate of Origin)
sign-off (`git commit -s`). See the
[DCO check workflow](.github/workflows/dco.yml) for details.

## Contact

Aaron Yourk — ayourk@gmail.com
