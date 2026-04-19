// SPDX-License-Identifier: GPL-3.0-only
//
// ActionRegistry — central QAction factory. Modeled on cctv-viewer's
// `actionDefs` registry: every menu action in the app is created
// and named here, then fetched by string ID from the menu builder.
// The current codebase uses only the core API (register, lookup);
// a future KeyBindingsDialog is planned to bolt on top, reading
// `defaultShortcut()` and writing a user override back into the
// registry without touching MainWindow code.
//
// The registry is a singleton. It owns the QAction instances —
// they outlive any individual QMenu / QMainWindow and are safe to
// share across windows in a future multi-window build. Actions
// are parented to the registry's internal QObject so Qt cleans
// them up at shutdown without caller intervention.
//
// ID conventions are `<category>.<verb>` with lowercase_snake:
//   "file.new_project", "edit.preferences", "view.toggle_editor",
//   "session.change_cwd", "help.about". Keep new IDs in sync with
//   the menus that consume them — breakage surfaces at runtime
//   as a null QAction*, which the DEBUG assert in action() turns
//   into a loud failure.

#pragma once

#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QObject>
#include <QString>

class QAction;

class ActionRegistry : public QObject {
    Q_OBJECT
public:
    static ActionRegistry& instance();

    // Register a new action under `id`. Returns the created
    // QAction, parented to the registry. `text` is the
    // user-visible label (with &-accelerators), `defaultShortcut`
    // is stored both on the QAction and as metadata for a future
    // KeyBindingsDialog to display the factory default. Returns
    // nullptr and logs a warning if `id` is already registered —
    // duplicate registration is a programming error, not a
    // silent-replace.
    QAction* registerAction(const QString& id,
                            const QString& text,
                            const QKeySequence& defaultShortcut = {},
                            const QString& statusTip = {});

    // Look up a previously-registered action by id. Asserts in
    // debug builds if the id is unknown; returns nullptr in
    // release builds so callers can defensively skip.
    QAction* action(const QString& id) const;

    // True if `id` has been registered.
    bool contains(const QString& id) const;

    // All registered ids in insertion order. Stable ordering
    // matters for a future KeyBindingsDialog that wants to list
    // actions in a deterministic sequence.
    QList<QString> actionIds() const;

    // Factory default shortcut for `id`, as provided at
    // registration time. Returns an empty QKeySequence if the id
    // has no default or isn't registered. This is the API the
    // future KeyBindingsDialog will call to render the
    // "reset to default" button next to each binding.
    QKeySequence defaultShortcut(const QString& id) const;

    // Clear the registry — only used by the test suite between
    // cases. Not part of the production lifecycle.
    void clearForTesting();

signals:
    void actionRegistered(const QString& id);

private:
    ActionRegistry();
    ActionRegistry(const ActionRegistry&) = delete;
    ActionRegistry& operator=(const ActionRegistry&) = delete;

    struct Entry {
        QAction* action = nullptr;
        QKeySequence defaultShortcut;
    };

    QHash<QString, Entry> m_entries;
    QList<QString> m_order;
};
