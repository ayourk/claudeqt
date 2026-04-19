// SPDX-License-Identifier: GPL-3.0-only
//
// EditorPaneWidget — tabbed container of CodeEditor instances.
// Owns the per-application KSyntaxHighlighting::Repository
// singleton and hands it out to each CodeEditor it constructs.
//
// Scope:
//   - QTabBar at top, QStackedWidget below
//   - openFile(path) → new tab with freshly-loaded CodeEditor
//   - closeTab(index) stashes dirty content and drops the page
//   - currentEditor() / saveCurrent() / saveAsCurrent() used by
//     File menu actions
//   - line wrap mode applied to every editor, current + future
//
// Deferred: buffer persistence to the `buffers` table requires a
// live session id. The hooks exist on CodeEditor (setBufferId) but
// are unused until session wiring lands with the tree model.

#pragma once

#include <QPlainTextEdit>
#include <QString>
#include <QWidget>

class QStackedWidget;
class QTabBar;
class CodeEditor;

namespace KSyntaxHighlighting {
class Repository;
}  // namespace KSyntaxHighlighting

class EditorPaneWidget : public QWidget {
    Q_OBJECT
public:
    explicit EditorPaneWidget(QWidget* parent = nullptr);
    ~EditorPaneWidget() override;

    static constexpr int kDefaultFontSize = 11;
    static constexpr int kDefaultTabWidth = 4;
    static const QString& defaultFontFamily();

    void setCurrentSessionId(qint64 sessionId);
    qint64 currentSessionId() const { return m_sessionId; }

    // Open `path` in a new tab and make it current. If `path`
    // is already open, raises the existing tab instead of
    // opening a duplicate. Returns the CodeEditor* on success,
    // or nullptr on I/O failure.
    CodeEditor* openFile(const QString& path);

    // Create an untitled (no filePath) editor in a new tab and
    // make it current. Tab title is "Untitled" (or "Untitled N"
    // when N>1 untitled tabs are already open). Save routes
    // through saveAs since filePath is empty.
    CodeEditor* newUntitled();

    // Close the tab at `index`, stashing any dirty content to
    // the persistence layer first if this editor is bound to a
    // buffer row. Idempotent on invalid index.
    void closeTab(int index);

    // Returns the CodeEditor in the currently-visible tab, or
    // nullptr if no tabs are open.
    CodeEditor* currentEditor() const;

    // Number of open tabs. Used by tests and by the toolbar.
    int tabCount() const;

    // Line wrap mode applied to all current and future editors.
    // Default: QPlainTextEdit::NoWrap per §6.2.
    void setGlobalLineWrapMode(QPlainTextEdit::LineWrapMode mode);
    QPlainTextEdit::LineWrapMode globalLineWrapMode() const {
        return m_wrapMode;
    }

    // Convenience wrappers used by the File menu.
    bool saveCurrent();
    bool saveAsCurrent(const QString& newPath);

    // Helpers for §5.3 change-cwd flow. The dirty guard
    // needs to know if any open tab has unsaved edits, and the
    // "Save all" / "Discard all" branches need a way to iterate
    // or wipe every tab without caring which one is current.
    void closePristineUntitledTabs();
    int dirtyEditorCount() const;
    bool saveAllDirty();   // returns false if any save failed
    void discardAllAndCloseAll();

    KSyntaxHighlighting::Repository* repository() const {
        return m_repository;
    }

    // Re-read editor.* settings from Persistence and
    // apply to every open editor plus cache for future opens.
    // Called at construction and from the settingChanged slot
    // whenever any `editor.*` key changes.
    void applyEditorSettings();

signals:
    // Emitted whenever the set of open tabs changes (open,
    // close, reorder). The MainWindow status bar listens so it
    // can update the dirty indicator.
    void tabsChanged();

private slots:
    void onTabBarCurrentChanged(int index);
    void onTabCloseRequested(int index);
    void onEditorFilePathChanged(const QString& newPath);
    void onEditorDirtyChanged(bool dirty);
    void onTabMoved(int from, int to);
    void onBuffersChanged(qint64 sessionId);

private:
    // Locate the tab index for an already-open CodeEditor, or
    // -1 if not present.
    int indexOfEditor(CodeEditor* editor) const;
    int indexOfPath(const QString& path) const;

    // Recompute tab title (`basename` + optional `*` suffix
    // for dirty) and tooltip (full path) for a given tab.
    void refreshTabLabel(int index);

    void restoreSessionBuffers();
    void flushAndCloseAllTabs();
    CodeEditor* editorForBufferId(qint64 bufferId) const;

    KSyntaxHighlighting::Repository* m_repository = nullptr;
    QTabBar* m_tabBar = nullptr;
    QStackedWidget* m_stack = nullptr;
    QPlainTextEdit::LineWrapMode m_wrapMode = QPlainTextEdit::NoWrap;
    qint64 m_sessionId = 0;
    bool m_suppressBufferSignal = false;
};
