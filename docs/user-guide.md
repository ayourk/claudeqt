# ClaudeQt User Guide

This guide covers the keyboard shortcuts, settings, and UI behavior
of the ClaudeQt desktop application.

> **Note:** Claude CLI integration is not yet wired. The chat shell
> accepts input and persists drafts, but does not send messages to
> Claude. Full interaction is planned for a future release. See
> [Release Notes](RELEASE_NOTES.md).

## Keyboard Shortcuts

| Action | Shortcut |
|---|---|
| New Window | Ctrl+Shift+N |
| New Project | Ctrl+Shift+P |
| New Session | Ctrl+N |
| Open File | Ctrl+O |
| Save | Ctrl+S |
| Save As | Ctrl+Shift+S |
| Undo | Ctrl+Z |
| Redo | Ctrl+Shift+Z |
| Toggle Editor Pane | Ctrl+E |
| Toggle Left Pane | Ctrl+L |
| Preferences | Ctrl+, |
| Quit | Ctrl+Q |
| Chat: Submit | Enter |
| Chat: New Line | Shift+Enter |
| Chat: Submit (alt) | Ctrl+Enter |

## Menu Bar

**File** — New Window, New Project, New Session, Open File, Save,
Save As, Close Session, Quit.

**Edit** — Undo, Redo, Cut, Copy, Paste, Preferences.

**View** — Toggle Editor, Toggle Left Pane, Reset Window Layout.

**Session** — Rename Session, Change Working Directory, Move to
Project, Delete Session.

**Help** — About ClaudeQt, GitHub repository link.

## Settings Dialog

Opened via Edit → Preferences or Ctrl+,.

### General Tab

| Setting | Default | Notes |
|---|---|---|
| Language | English | English and Chinese available; change requires restart |
| Theme variant | Dark | Dark, Light, or System; change currently requires restart |
| Default new-session cwd | `$HOME` | Working directory for new sessions; line edit + Browse button |
| Confirm on session delete | Enabled | Show confirmation dialog before deleting sessions |
| Confirm on project delete | Enabled | Show confirmation dialog before deleting projects |
| Project sort order | Last used | Last used, Name, or Off |
| Session sort order | Last used | Last used, Title, or Off |
| Activation dwell | 1000 ms | Range: 0–30000 ms; delay before a session click reorders the tree |
| Reset Window Layout | — | Button; resets all geometry to compiled-in defaults |

### Editor Tab

| Setting | Default | Notes |
|---|---|---|
| Font family | Monospace | Any installed monospace font |
| Font size | 11 | Range: 8–32 |
| Tab width | 4 | Range: 1–8 |
| Insert spaces for tab | Enabled | Soft tabs |
| Line wrap mode | No wrap | No wrap or Soft wrap at viewport edge |
| Show line numbers | Enabled | Gutter line numbers |
| Highlight current line | Enabled | Subtle background highlight on cursor line |
| Chat input min rows | 2 | Range: 1 through 5 |
| Chat input max rows | 10 | (min rows) through 20, or "No limit" (auto-grow stops at 50% of available space) |

### About Tab

Shows version, dependency versions, platform information, and links
to the PPA, source repository, and issue tracker.

## Window Layout

The window has three main areas:

- **Left pane** — project/session tree with a "Settings…" button
  at the bottom (toggle with Ctrl+L)
- **Top-right** — tabbed code editor (toggle with Ctrl+E)
- **Bottom-right** — message view and chat input

### Splitter Handles

All dividers between panes are draggable. Right-clicking a splitter
handle resets it:

- **Left/right divider** — resets left pane to default width (220 px)
- **Editor/message divider** — resets to 50/50 split
- **Message/chat divider** — toggles between auto-grow mode and the
  last manual size

### Geometry Persistence

Window size, position, splitter ratios, pane visibility, and tree
expansion state are saved automatically (500 ms debounce) and
restored on next launch. Splitter positions are per-window — dragging
a divider on one window does not affect others.

Default window size is 800 × 600 pixels. Minimum is 640 × 400.

## Project & Session Tree

### Projects

Projects are hierarchical folders that group sessions. They can nest
to arbitrary depth. Each project has a name and a root directory path.

**Creating:** File → New Project (Ctrl+Shift+N). Provide a name,
root path, and optional parent project.

**Right-click menu:** Rename, New Sub-Project, New Session, Delete.

**Drag-and-drop:** Drag a project onto another project to reparent
it. Drag to the root level to make it top-level.

### Sessions

Sessions belong to a project or exist as orphans at the root level.
Each session has a title, a retargetable working directory, message
history, and open file buffers.

**Creating:** File → New Session (Ctrl+N). Uses the selected
project's root as the working directory, or `$HOME` for orphan
sessions.

**Right-click menu:** Rename, Change Working Directory, Move to
Project, Delete.

**Retargeting the working directory:** Session → Change Working
Directory. All open editor tabs close after the retarget. If any
tabs have unsaved changes, a prompt appears first:

- **Save all and retarget** — saves every modified buffer, then
  closes all tabs and changes the working directory. If any buffer
  is untitled (never saved to disk), the retarget aborts — save
  the untitled buffer via Save As first.
- **Discard and retarget** — discards unsaved changes, closes all
  tabs (including unsaved Untitled buffers), and changes the working directory.
- **Cancel** — aborts the retarget; no tabs are closed, no changes
  are lost.

If all tabs are already saved, no prompt appears — tabs close and
the working directory changes immediately.

**Sort order:** Configurable in Settings → General. "Last used"
bubbles recently activated sessions (and their ancestor projects)
to the top of their sibling lists. "Off" preserves database
insertion order without reordering.

### Activation Dwell

Clicking rapidly through sessions does not reorder the tree
immediately. A dwell timer (1 second) prevents the tree from
shuffling under the cursor during fast navigation.

## Code Editor

### Opening Files

- File → Open (Ctrl+O) — file dialog rooted at the session's
  working directory
- Drag a file from a file manager into the window
- Session switch restores previously open buffers

### Tabs

- `*` suffix on the tab title indicates unsaved changes
- Tooltip shows the full absolute file path
- Close button on each tab; closing a tab with unsaved changes
  prompts Save / Discard / Cancel

### Saving

- Ctrl+S saves to the current path
- Ctrl+Shift+S opens a Save As dialog
- Untitled buffers prompt for a path on first save

### External Modification Detection

When the window regains focus, ClaudeQt checks whether open files
were modified externally. Files without unsaved changes reload
silently. Files with unsaved changes show a prompt: "Keep my edits"
or "Reload and lose my edits."

### Syntax Highlighting

Powered by KSyntaxHighlighting with 300+ language definitions.
Language detection is automatic based on file extension and MIME type.

## Chat Shell

### Input

- **Enter** submits the message
- **Shift+Enter** inserts a newline
- **Ctrl+Enter** also submits (alternative binding)
- Input area auto-grows as you type, starting at the min row
  limit and growing up to the max row limit configured in
  Settings → Editor
- Auto-grow is also capped at 50% of the available vertical
  space, whichever limit is reached first
- Setting max rows to "No limit" (0) removes the row cap;
  auto-grow stops only at the 50% cap
- Drag the message/chat divider to override auto-grow with a manual
  size; right-click the handle to toggle back to auto mode

### Draft Persistence

Unsent text in the chat input is saved per session and restored on
session switch or app restart. Drafts survive crashes (500 ms flush
interval).

### Large Paste Handling

Pasting text larger than 8 KB spills the content into a temporary
attachment file with a reference token inserted into the input field.
This prevents the input widget from choking on large payloads.

## Status Bar

| Section | Content |
|---|---|
| Left | Active session's working directory |
| Right | Active session name |

## Multi-Window Support

Open a new window via File → New Window (Ctrl+Shift+N) or by
launching a second instance of ClaudeQt. Multiple windows run
simultaneously, sharing one SQLite database. A leader-elected IPC
hub over Unix domain sockets keeps windows in sync automatically.

**Synced across windows:**

- Project and session tree changes (create, rename, delete, reparent)
- Session activation (updates tree sort order in all windows)
- Messages appended to a session (per session)
- Open file buffer changes (per session)
- Draft text updates (per session)
- Global settings (sort order, font, theme, editor preferences)

Two windows on the same session share drafts, messages, and
open file tabs in near-real-time. Two windows on different
sessions are independent — session-scoped events (messages,
buffers, drafts) only reach windows viewing that session.

**Per-window state** (not synced): which session is selected,
window geometry, splitter positions, pane visibility. Each window
remembers its own layout and active session independently.

If the hub leader process exits, the remaining windows re-elect a
new leader and resynchronize automatically.

## Theming

ClaudeQt ships a dark theme applied via QSS (Qt Style Sheets) with
a semantic color palette. Theme variant (Dark / Light / System) is
configurable in Settings → General. Changing the theme currently
requires a restart; live switching is planned for a future release.
