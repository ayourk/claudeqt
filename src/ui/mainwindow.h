// SPDX-License-Identifier: GPL-3.0-only
//
// MainWindow — top-level GUI shell.
//
// Layout scaffolding: outer splitter + left pane + right column +
// menu bar + status bar + toggle slots. Additional capabilities:
//   - RightClickSplitter for outer and top splitters, with
//     right-click handlers that reset to compiled-in defaults
//     and persist the reset value to settings_kv (§3.2)
//   - geometry persistence to settings_kv with debounced
//     write-through (§3.4) for window size/pos/maximized state,
//     left-pane width + visibility, top-splitter ratio, and
//     editor visibility
//   - View → Reset Window Layout action with ThemedQtDialog
//     confirmation that wipes the geometry keys
//
// Deliberately deferred:
//   - chat_splitter.* keys and auto-resize logic (needs the
//     chat shell)
//   - tree.expanded_ids + session.last_active_id (needs the
//     tree model)
//   - real EditorPaneWidget with KSH (§6)
//   - real ChatShellWidget and messageChatSplitter (§7)
//   - SettingsDialog (§8.2)
//   - chat_input.rows key (lives with Settings)

#pragma once

#include <QMainWindow>

class QAction;
class QCloseEvent;
class QLabel;
class QMoveEvent;
class QPlainTextEdit;
class QPoint;
class QResizeEvent;
class QSplitter;
class QTimer;
class QWidget;
class QItemSelection;
class QModelIndex;
class QTreeView;
class CodeEditor;
class ChatInputWidget;
class EditorPaneWidget;
class MessageWindowWidget;
class ProjectSessionTreeModel;
class RightClickSplitter;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // Compile-time minimum window dimensions. Exposed as static
    // constexpr so tests can assert on them without hardcoding
    // the literals in two places.
    static constexpr int kMinWidth = 640;
    static constexpr int kMinHeight = 400;

    // Compiled-in defaults for right-click "reset to default" on
    // the outer and top splitters (§3.2), and for
    // applyDefaultGeometry() on Reset Window Layout.
    static constexpr int kDefaultWidth = 800;
    static constexpr int kDefaultHeight = 600;
    static constexpr int kDefaultLeftPaneWidth = 220;
    static constexpr double kDefaultTopMessageRatio = 0.5;
    static constexpr int kDefaultActivationDwellMs = 1000;
    static constexpr int kDefaultChatInputMinRows = 2;
    static constexpr int kDefaultChatSplitterMinBottomRows = 5;
    static constexpr int kDefaultChatInputMaxRows = 10;

    // Accessors used by the QtTest suite to poke at internal
    // widgets without cracking the class open.
    QSplitter* outerSplitter() const;
    QSplitter* topSplitter() const;
    QSplitter* messageChatSplitter() const;
    QWidget* leftPane() const { return m_leftPane; }
    QWidget* editorPane() const { return m_editorPane; }
    EditorPaneWidget* editorPaneWidget() const;
    MessageWindowWidget* messageWindow() const { return m_messageWindow; }
    ChatInputWidget* chatInputWidget() const { return m_chatInputWidget; }

    // The currently-active session id. Bootstrapped to a scratch
    // session so the chat shell works end-to-end without the tree
    // model; tree-driven session selection replaces the bootstrap.
    qint64 currentSessionId() const { return m_currentSessionId; }

    // Open `path` in a new CodeEditor tab. Ensures the editor
    // pane is visible first (expanding if hidden), then delegates
    // to EditorPaneWidget::openFile. Returns nullptr on I/O error.
    // Exposed publicly so tests can drive the load path without
    // going through QFileDialog.
    CodeEditor* openFile(const QString& path);

public slots:
    // Toggle the editor pane (top-right slot) between hidden and
    // 50/50 with the message view. Triggered by View → Toggle
    // Editor, Ctrl+E, and (later) the button-bar toggle button.
    // Persists `editor.visible` immediately (no debounce) since
    // toggles are discrete user actions per §3.4.
    void toggleEditor();

    // Toggle the entire left pane between hidden and its restored
    // width. Triggered by View → Toggle Left Pane and Ctrl+L.
    // Persists `left_pane.visible` immediately.
    void toggleLeftPane();

    // Right-click handlers for the outer + top splitters. Snap
    // the splitter to the compiled-in default and overwrite the
    // persisted value, so the reset survives relaunch.
    void resetLeftPaneWidth();
    void resetTopMessageRatio();

    // View → Reset Window Layout. Prompts for confirmation via a
    // ThemedQtDialog, then wipes every geometry key and calls
    // applyDefaultGeometry().
    void resetWindowLayout();

    // Apply the compiled-in defaults to the live widgets without
    // touching settings_kv. Called from resetWindowLayout() after
    // the keys are wiped, and from the load path when the DB has
    // no geometry yet.
    void applyDefaultGeometry();

    // File menu handlers for the editor subsystem (§6.3). Hooked
    // to File → Open / Save / Save As and the Ctrl+O/S/Shift+S
    // shortcuts. openFileDialog() routes a QFileDialog selection
    // through openFile() above.
    void openFileDialog();
    void saveCurrentFile();
    void saveCurrentFileAs();

    // Open the modal SettingsDialog centered on this window.
    // Wired from Edit → Preferences and the left pane's
    // Settings… button.
    void openSettingsDialog();

protected:
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    // File-URL drops fall through from ChatInputWidget per
    // §7.3 — MainWindow's dropEvent opens the file in the
    // editor, unifying the gesture with drop-on-editor per
    // §6.3 item 3.
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void buildCentralWidget();
    void buildMenuBar();
    void buildStatusBar();
    void installShortcuts();
    void installGeometryTimers();

    QWidget* buildLeftPane();
    QWidget* buildRightColumn();

    // settings_kv load/save. Each is idempotent; load() applies
    // whatever it finds (defaults for missing keys), save*() only
    // writes the relevant subset.
    void loadGeometryFromSettings();
    void saveWindowGeometryNow();
    void saveWindowPositionNow();
    void saveLeftPaneWidthNow();
    void saveTopSplitterRatioNow();
    void applyTopSplitterRatio(double ratio = -1.0);

    void flushPendingGeometryWrites();

    // Slot callbacks for splitterMoved signals. They start the
    // per-splitter debounce timer; the timer fires into
    // save*Now().
    void onOuterSplitterMoved();
    void onTopSplitterMoved();

    RightClickSplitter* m_outerSplitter = nullptr;
    RightClickSplitter* m_topSplitter = nullptr;
    RightClickSplitter* m_messageChatSplitter = nullptr;

    QWidget* m_leftPane = nullptr;
    QWidget* m_rightColumn = nullptr;
    QWidget* m_buttonBar = nullptr;
    QWidget* m_messagePane = nullptr;
    QWidget* m_editorPane = nullptr;
    MessageWindowWidget* m_messageWindow = nullptr;
    ChatInputWidget* m_chatInputWidget = nullptr;

    QLabel* m_statusCwdLabel = nullptr;
    QLabel* m_statusSessionIdLabel = nullptr;

    // Debounce timers, one per independent key group. All are
    // single-shot, restarted on every matching event.
    QTimer* m_windowGeometryTimer = nullptr;
    QTimer* m_windowPositionTimer = nullptr;
    QTimer* m_leftPaneWidthTimer = nullptr;
    QTimer* m_topSplitterRatioTimer = nullptr;

    // Cached width so toggleLeftPane can restore the pane at its
    // last-visible width after a hide-then-show round trip. Stays
    // >= 1 to avoid a zero-width restore that looks like nothing
    // happened.
    int m_savedLeftPaneWidth = kDefaultLeftPaneWidth;

    // Suppresses the `saveLeftPaneWidthNow()` path while
    // loadGeometryFromSettings() and applyDefaultGeometry() are
    // programmatically resizing splitters. Without it, the
    // programmatic `setSizes()` calls trigger `splitterMoved`
    // signals, which would debounce-write back the same value we
    // just read — harmless but confusing when tests inspect the
    // call graph.
    bool m_suppressSplitterWrites = false;

    // --- Chat shell --------------------------------------------
    //
    // The currently-active session. Bootstrapped by
    // ensureScratchSessionExists() on startup so the chat flow
    // has an id to bind to. Tree-driven session switching
    // replaces the bootstrap.
    qint64 m_currentSessionId = 0;

    // Dwell timer: defers the activateSession() call (which bumps
    // last_used and triggers a sort reorder) until the user has
    // stayed on a session for at least N milliseconds. Prevents
    // the tree from reordering under the cursor during fast
    // click-through navigation.
    QTimer* m_activationDwellTimer = nullptr;
    qint64 m_pendingDwellSessionId = 0;

    // Auto/manual state for the messageChatSplitter per §7.2.
    // `m_chatSplitUserOverridden` flips to true on any explicit
    // user drag. `m_chatSplitStashedManualSize` remembers the
    // last manual size so a right-click can flip back to it.
    bool m_chatSplitUserOverridden = false;
    int m_chatSplitStashedManualSize = -1;
    // Guard against re-entering recomputeChatSplitAuto() from
    // its own setSizes() call, which would emit splitterMoved
    // and flip m_chatSplitUserOverridden to true spuriously.
    bool m_suppressChatSplitterSignal = false;

    // Chat splitter persistence per §3.4 event table.
    // Debounce timer coalesces a drag into a single settings_kv
    // write, matching the 500 ms rule used by the other splitter
    // timers. `m_chatSplitPendingManualSize` carries the persisted
    // manual_size from load-time until the deferred validation
    // pass applies it (splitterH is zero until the first layout
    // pass, so clamping has to wait).
    QTimer* m_chatSplitterWriteTimer = nullptr;
    int m_chatSplitPendingManualSize = -1;
    int m_chatSplitPendingStashedManualSize = -1;
    bool m_chatSplitNeedsValidation = false;
    bool m_topSplitterNeedsApply = false;

    void ensureScratchSessionExists();
    void recomputeChatSplitAuto();
    void onMessageChatSplitterMoved(int pos, int index);
    void onMessageChatSplitterRightClicked();

    // Persistence hooks for chat_splitter.* keys.
    // saveChatSplitterStateNow() writes both manual_size and
    // stashed_manual_size based on the live runtime state.
    // validateAndApplyChatSplitterState() runs once on the first
    // layout pass to clamp a persisted manual_size into the
    // child-minimum band and to drop a persisted stashed value
    // that exceeds the 50%/row-cap rule.
    void saveChatSplitterStateNow();
    void validateAndApplyChatSplitterState();

public:
    // Child-minimum band for the chat splitter, exposed so the
    // test suite can assert the clamp targets
    // deterministically. kChatSplitterMinTopPx is a compile-time
    // constant; the bottom minimum depends on the current
    // font and `chat_input.min_rows`.
    int chatSplitterMinTopHeight() const;
    int chatSplitterMinBottomHeight() const;
    int chatSplitterRowCap() const;

private:

    // --- Tree model ---------------------------------------------
    //
    // The left pane's project/session tree backed by Persistence,
    // with selection changes wired through to the chat shell's
    // active session.
    QTreeView* m_projectTree = nullptr;
    ProjectSessionTreeModel* m_treeModel = nullptr;

    void setActiveSession(qint64 sessionId);
    void onTreeSelectionChanged(const QItemSelection& selected,
                                const QItemSelection& deselected);
    void saveExpandedIdsNow();
    void restoreExpandedIds();
    QTimer* m_expandedIdsTimer = nullptr;

    // Guards the modelAboutToBeReset snapshot against
    // overwriting the persisted tree.expanded_ids before the
    // startup restoreExpandedIds() call has had a chance to load
    // it. Flips to true the first time restoreExpandedIds() runs.
    bool m_treeExpandedIdsRestored = false;

    // --- Tree CRUD ----------------------------------------------
    //
    // Slot bodies for File → New Project / New Session and the
    // Session menu actions, plus the right-click context menu on
    // the tree. All dispatch to Persistence through
    // ThemedQtDialog-confirmed flows.
    void onNewWindow();
    void onNewProject();
    void onNewSession();
    void onRenameSelected();
    void onDeleteSelected();
    void onChangeSessionCwd();
    void onMoveSessionToProject();
    void onTreeContextMenu(const QPoint& pos);

    // Helpers shared between the context menu and the File/Session
    // menu handlers.
    qint64 selectedProjectId() const;
    qint64 selectedSessionId() const;
    QModelIndex selectedIndex() const;

    // §5.3 dirty-buffer gate for retargetSessionCwd. Returns true
    // if the caller should proceed with the UPDATE, false if the
    // user cancelled. `doSave` is set to true if the user picked
    // "Save all and retarget" so the caller can write buffers
    // before the UPDATE lands.
    bool confirmCwdRetarget(int dirtyCount, bool& doSave);
};
