# ClaudeQt architecture

> This file is a **Phase 1 stub**. The real architecture document
> lands alongside the Phase 2 DAO/schema work. For the canonical
> design see [docs/phase-1.md](docs/phase-1.md).

## Process model

ClaudeQt is two binaries from one source package:

- **`claudeqt`** — Qt6 Widgets GUI, one process per user session. Holds
  the canonical store for all session data (transcripts, projects,
  file edits, tool output history) in a SQLite database under
  `$XDG_DATA_HOME/claudeqt/`.
- **`claudeqt-remote`** — stateless headless worker. Accepts
  TLS/JSON-RPC connections from the GUI, receives per-turn session
  state, execs the Claude Code CLI, and streams results back. Ships
  as a dedicated `systemd` unit running under the `claudeqt` system
  user. Canonical session data lives on the GUI side, not here.

Both binaries link a shared `ClaudeCliLocator` class for path
discovery, version gating (≥ 2.1.97), and `claude auth status`
probing. Same code, same fail-closed policy.

## Dependency floors

| Component | Floor | Rationale |
|---|---|---|
| Qt6 | 6.4.2 | Ubuntu Noble stock |
| KF5 SyntaxHighlighting (Qt6) | 5.115.0 | Shipped via PPA |
| KF6 SyntaxHighlighting | (any) | Resolute future path |
| libcmark-gfm | (system) | pkg-config |
| SQLite3 | 3.45 | `RETURNING`, `JSON_*` |
| Python3 | 3.12 | embedded Python bridge |
| libsystemd | (system) | `sd_notify` on daemon |
| libzstd | 1.5.5 | blob compression |

## Transport (Phase 6)

- TLS over TCP, Ed25519 self-signed certs generated in postinst
- JSON-RPC 2.0 frames on the control plane (uncompressed)
- Binary blob channel for workspace snapshots and tool output (zstd)
- Default port: 49428 (dynamic range, no IANA assignment)
- Default bind: `127.0.0.1`; expose via `remote.conf` + systemd drop-in

## Repository layout

See [docs/phase-1.md §5.2](docs/phase-1.md).
