# ClaudeQt Release Notes

## Development Roadmap

| Stage | Goal | Status |
|---|---|---|
| 1 | Foundation & dependency freeze | Complete |
| 2 | GUI shell (layout, persistence, editor, chat, tree, settings, theming, multi-instance) | Complete |
| 3 | Claude CLI integration (JSON-RPC streaming, permission handling, tool use display) | Planned |
| 4 | Advanced features (MCP (Model Context Protocol) integration, context ring, usage stats) | Planned |
| 5 | Remote daemon (TLS transport, authentication, session relay) | Planned |
| 6 | Cross-platform (macOS, Windows) | Complete |

---

## 0.0.1 — Foundation

Validated the full build, packaging, and distribution pipeline end
to end before any real application code was written.

- **Build system** — CMake + Ninja build graph linking Qt6,
  KSyntaxHighlighting (Qt6 backport via PPA), cmark-gfm, SQLite3,
  and libsystemd
- **Packaging** — three `.deb` packages (`claudeqt`, `claudeqt-remote`,
  `claudeqt-common`) validated via Docker-based clean-chroot build
  (`ubuntu:noble` container), `ldd` audit, and `autopkgtest`
- **PPA** — `ppa:ayourk/claudeqt` published with the
  `kf5syntaxhighlighting-qt6` backport built by Launchpad for
  Noble amd64 + arm64
- **CI** — GitHub Actions workflows for Linux, Windows (MSVC), macOS,
  and DCO sign-off checks
- **Daemon skeleton** — `claudeqt-remote` as a Type=notify systemd
  service with watchdog liveness, dedicated system user, self-signed
  Ed25519 TLS cert generation in postinst
- **Dependency floors** — pinned minimum versions for all dependencies
  against Ubuntu Noble stock packages

---

## 0.0.1 — GUI Shell

> Version 0.0.1 covers the foundation and GUI shell stages. The
> version number will bump when the first functional Claude CLI
> integration ships.

### What's in the package

- **`claudeqt`** — full GUI shell: project/session tree, tabbed code
  editor with KSyntaxHighlighting, chat input with draft persistence,
  settings dialog, dark theme, geometry persistence, and multi-window
  support via leader-elected IPC hub
- **`claudeqt-remote`** — Type=notify daemon with systemd watchdog
  liveness. TLS transport and JSON-RPC session relay are planned for
  a future release
- **`claudeqt-common`** — shared resources (icons, desktop entry)

### Cross-platform

- CI workflows for Debian, AppImage, Arch, RPM, Flatpak,
  Windows MSVC, and macOS universal
- vcpkg static builds for Windows and macOS via custom overlay
  registries (claudeqt-vcpkg, hobbycad-vcpkg)
- macOS app bundle with auto-derived bundle ID

### Fork-friendly

- `project_details()` macro in CMakeLists.txt: app name, version,
  about box, homepage, issues URL, PPA, macOS bundle ID, and IPC
  socket name all derive from it — no source code edits needed
- `ISSUES_URL` supports `@HOMEPAGE_URL@` substitution
- PPA field is optional and only shown on Debian-based systems

### What's not yet in the package

- Claude CLI interaction (chat submission, streaming responses,
  tool use display)
- Markdown rendering in the message view
- MCP integration, slash commands, context ring
- Remote daemon session relay

### Installation

```sh
sudo add-apt-repository ppa:ayourk/claudeqt
sudo apt update
sudo apt install claudeqt          # GUI only
sudo apt install claudeqt-remote   # Headless daemon
```

The Claude Code CLI must be installed separately. After installing
the `claudeqt` package, see `/usr/share/doc/claudeqt/README.Debian`
for setup instructions.
