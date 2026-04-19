// SPDX-License-Identifier: GPL-3.0-only
#include "mainwindow.h"

#include "actionregistry.h"
#include "chatinputwidget.h"
#include "codeeditor.h"
#include "editorpanewidget.h"
#include "messagewindowwidget.h"
#include "persistence.h"
#include "projectsessiontreemodel.h"
#include "rightclicksplitter.h"
#include "settingsdialog.h"
#include "themedqtdialog.h"

#include <QAction>
#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QByteArray>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QIcon>
#include <QPixmap>
#include <QItemSelection>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QMoveEvent>
#include <QProcess>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QSet>
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QString>
#include <QStringList>
#include <QStatusBar>
#include <QTextDocument>
#include <QTimer>
#include <QToolBar>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>
#include <algorithm>
#include <functional>

namespace {

// --- settings_kv typed accessors --------------------------------
//
// Persistence::setSetting/getSetting take QByteArray. Geometry
// values are ints / doubles / bools. These tiny helpers translate
// via QVariant so the call sites stay readable. These can be
// pulled into a shared `settings_kv.h` header if a second
// consumer materializes.

int readInt(const QString& key, int fallback) {
    const auto blob = Persistence::instance().getSetting(key);
    if (blob.isEmpty()) return fallback;
    bool ok = false;
    const int v = QString::fromUtf8(blob).toInt(&ok);
    return ok ? v : fallback;
}

void writeInt(const QString& key, int value) {
    Persistence::instance().setSetting(
        key, QByteArray::number(value));
}

bool readBool(const QString& key, bool fallback) {
    const auto blob = Persistence::instance().getSetting(key);
    if (blob.isEmpty()) return fallback;
    // Accept both "1"/"0" and "true"/"false" so hand-edited rows
    // from a DBA session still work.
    const auto s = QString::fromUtf8(blob).trimmed().toLower();
    if (s == QStringLiteral("1") || s == QStringLiteral("true"))
        return true;
    if (s == QStringLiteral("0") || s == QStringLiteral("false"))
        return false;
    return fallback;
}

void writeBool(const QString& key, bool value) {
    Persistence::instance().setSetting(
        key, value ? QByteArray("1") : QByteArray("0"));
}

double readDouble(const QString& key, double fallback) {
    const auto blob = Persistence::instance().getSetting(key);
    if (blob.isEmpty()) return fallback;
    bool ok = false;
    const double v = QString::fromUtf8(blob).toDouble(&ok);
    return ok ? v : fallback;
}

void writeDouble(const QString& key, double value) {
    Persistence::instance().setSetting(
        key, QByteArray::number(value, 'g', 10));
}

void clearKey(const QString& key) {
    Persistence::instance().clearSetting(key);
}

// Debounce interval for all geometry writers per §3.4. 500 ms is
// conservative enough to coalesce a drag, tight enough that the
// worst-case power loss loses at most half a second of trailing
// tweaks.
constexpr int kGeometryDebounceMs = 500;

// settings_kv key names. Centralized so a typo hits one place
// and tests can reference the same constants.
const QString kKeyWindowWidth = QStringLiteral("window.width");
const QString kKeyWindowHeight = QStringLiteral("window.height");
const QString kKeyWindowX = QStringLiteral("window.x");
const QString kKeyWindowY = QStringLiteral("window.y");
const QString kKeyWindowMaximized = QStringLiteral("window.maximized");
const QString kKeyLeftPaneWidth = QStringLiteral("left_pane.width");
const QString kKeyLeftPaneVisible = QStringLiteral("left_pane.visible");
const QString kKeyTopSplitterRatio =
    QStringLiteral("top_splitter.message_ratio");
const QString kKeyEditorVisible = QStringLiteral("editor.visible");
// Chat splitter persistence keys per §3.4 event table.
// manual_size: -1 = auto mode; ≥0 = manual override bottom-child
// pixel height. stashed_manual_size: -1 = empty stash; ≥0 =
// remembered manual size for right-click toggle back to manual.
const QString kKeyChatSplitterManualSize =
    QStringLiteral("chat_splitter.manual_size");
const QString kKeyChatSplitterStashedManualSize =
    QStringLiteral("chat_splitter.stashed_manual_size");
// Tree persistence keys.
const QString kKeyTreeExpandedIds =
    QStringLiteral("tree.expanded_ids");
const QString kKeySessionLastActiveId =
    QStringLiteral("session.last_active_id");

// Every key touched by View → Reset Window Layout. Keep this in
// sync with the load path and with applyDefaultGeometry(); the
// test suite walks the list to assert post-reset state.
const QStringList kGeometryKeys = {
    kKeyWindowWidth,     kKeyWindowHeight, kKeyWindowX,
    kKeyWindowY,         kKeyWindowMaximized,
    kKeyLeftPaneWidth,   kKeyLeftPaneVisible,
    kKeyTopSplitterRatio, kKeyEditorVisible,
    kKeyChatSplitterManualSize, kKeyChatSplitterStashedManualSize,
};

// §3.4 + §7.2: the chat splitter has two child minimums. The top
// (message view) must be ≥ 120 px so the inert banner plus ~3
// lines of history remain visible. The bottom (chat input)
// minimum is derived at runtime from `min_rows × lineHeight +
// padding` via chatSplitterMinBottomHeight() below.
constexpr int kChatSplitterMinTopPx = 120;

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QApplication::applicationName());
    setMinimumSize(kMinWidth, kMinHeight);

    buildCentralWidget();
    buildMenuBar();
    buildStatusBar();
    installShortcuts();
    installGeometryTimers();

    // Apply defaults first, then overlay anything the DB has to
    // say. Split in two phases so a partially populated
    // settings_kv (e.g., only `window.width` present) still gets
    // sane values for the keys it lacks.
    applyDefaultGeometry();
    loadGeometryFromSettings();

    ensureScratchSessionExists();

    // Post a deferred auto-split recompute so the splitter has
    // had one layout pass before we try to size it. Without the
    // defer, the splitter's height() is still 0 and the clamp
    // degenerates. Validation runs first so a
    // persisted manual_size gets clamped + applied before the
    // auto-split path runs.
    QTimer::singleShot(0, this, [this]() {
        if (m_topSplitterNeedsApply) {
            m_topSplitterNeedsApply = false;
            if (m_topSplitterRatioTimer &&
                m_topSplitterRatioTimer->isActive()) {
                m_topSplitterRatioTimer->stop();
            }
            m_suppressSplitterWrites = true;
            applyTopSplitterRatio();
            m_suppressSplitterWrites = false;
        }
        if (m_chatSplitNeedsValidation) {
            validateAndApplyChatSplitterState();
        } else {
            recomputeChatSplitAuto();
        }
    });

    // File URL drop target for the editor per §6.3 item 3.
    setAcceptDrops(true);

    // Listen for SettingsDialog's "reset layout" marker.
    // The dialog can't own the MainWindow, so it writes a setting
    // and we react here. Guard against the clearSetting feedback
    // loop (clearSetting also emits settingChanged).
    connect(&Persistence::instance(), &Persistence::settingChanged,
            this, [this](const QString& name) {
                if (name == QStringLiteral("session.activation_dwell_ms")) {
                    if (m_activationDwellTimer) {
                        const QByteArray raw =
                            Persistence::instance().getSetting(name);
                        int ms = kDefaultActivationDwellMs;
                        if (!raw.isEmpty()) {
                            bool ok = false;
                            const int v =
                                QString::fromUtf8(raw).toInt(&ok);
                            if (ok && v >= 0) ms = v;
                        }
                        m_activationDwellTimer->setInterval(ms);
                    }
                    return;
                }
                static bool inResetHandler = false;
                if (inResetHandler) return;
                if (name != QStringLiteral("window.reset_requested")) {
                    return;
                }
                inResetHandler = true;
                Persistence::instance().clearSetting(
                    QStringLiteral("window.reset_requested"));
                inResetHandler = false;
                resetWindowLayout();
            });
}

MainWindow::~MainWindow() = default;

QSplitter* MainWindow::outerSplitter() const { return m_outerSplitter; }
QSplitter* MainWindow::topSplitter() const { return m_topSplitter; }
QSplitter* MainWindow::messageChatSplitter() const {
    return m_messageChatSplitter;
}

// --- Layout construction ----------------------------------------

void MainWindow::buildCentralWidget() {
    m_outerSplitter = new RightClickSplitter(Qt::Horizontal, this);
    m_outerSplitter->setObjectName(QStringLiteral("outerSplitter"));
    m_outerSplitter->setChildrenCollapsible(false);
    m_outerSplitter->setHandleWidth(5);

    m_leftPane = buildLeftPane();
    m_rightColumn = buildRightColumn();

    m_outerSplitter->addWidget(m_leftPane);
    m_outerSplitter->addWidget(m_rightColumn);

    m_outerSplitter->setStretchFactor(0, 0);
    m_outerSplitter->setStretchFactor(1, 1);
    m_outerSplitter->setSizes({m_savedLeftPaneWidth, 800});

    connect(m_outerSplitter, &QSplitter::splitterMoved, this,
            &MainWindow::onOuterSplitterMoved);
    connect(m_outerSplitter, &RightClickSplitter::handleRightClicked,
            this, &MainWindow::resetLeftPaneWidth);

    setCentralWidget(m_outerSplitter);
}

QWidget* MainWindow::buildLeftPane() {
    auto* pane = new QWidget(this);
    pane->setObjectName(QStringLiteral("leftPane"));
    pane->setMinimumWidth(180);

    auto* layout = new QVBoxLayout(pane);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* topToolbar = new QToolBar(pane);
    topToolbar->setObjectName(QStringLiteral("leftPaneTopToolbar"));
    topToolbar->setFixedHeight(0);
    layout->addWidget(topToolbar);

    m_treeModel = new ProjectSessionTreeModel(this);
    m_projectTree = new QTreeView(pane);
    m_projectTree->setObjectName(QStringLiteral("projectSessionTree"));
    m_projectTree->setHeaderHidden(true);
    m_projectTree->setModel(m_treeModel);
    m_projectTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_projectTree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_projectTree->setDragEnabled(true);
    m_projectTree->setAcceptDrops(true);
    m_projectTree->setDropIndicatorShown(true);
    m_projectTree->setDragDropMode(QAbstractItemView::InternalMove);
    m_projectTree->setDefaultDropAction(Qt::MoveAction);

    // Right-click context menu for CRUD actions and
    // inline rename via double-click / F2 (model exposes
    // Qt::ItemIsEditable).
    m_projectTree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_projectTree->setEditTriggers(QAbstractItemView::DoubleClicked |
                                   QAbstractItemView::EditKeyPressed |
                                   QAbstractItemView::SelectedClicked);
    connect(m_projectTree, &QTreeView::customContextMenuRequested, this,
            &MainWindow::onTreeContextMenu);

    connect(m_projectTree->selectionModel(),
            &QItemSelectionModel::selectionChanged, this,
            &MainWindow::onTreeSelectionChanged);
    connect(m_projectTree, &QTreeView::expanded, this,
            [this](const QModelIndex&) {
                if (m_expandedIdsTimer) m_expandedIdsTimer->start();
            });
    connect(m_projectTree, &QTreeView::collapsed, this,
            [this](const QModelIndex&) {
                if (m_expandedIdsTimer) m_expandedIdsTimer->start();
            });

    // Expansion state must survive the full model reset
    // cycle that a sort-order change (or any rebuildTree) fires.
    // modelAboutToBeReset runs while the old TreeNode pointers are
    // still live in the view, so saveExpandedIdsNow() can walk
    // isExpanded() against them and flush the current state to
    // settings_kv synchronously. modelReset runs against the fresh
    // tree, so restoreExpandedIds() can re-expand from the
    // just-flushed value. The round-trip through settings_kv keeps
    // the in-memory and persisted states in sync, which matters if
    // the app dies mid-resort.
    connect(m_treeModel, &QAbstractItemModel::modelAboutToBeReset,
            this, [this]() {
                if (!m_treeExpandedIdsRestored) return;
                if (m_expandedIdsTimer && m_expandedIdsTimer->isActive()) {
                    m_expandedIdsTimer->stop();
                }
                saveExpandedIdsNow();
            });
    connect(m_treeModel, &QAbstractItemModel::modelReset, this,
            [this]() {
                if (!m_treeExpandedIdsRestored) return;
                restoreExpandedIds();
                if (m_currentSessionId > 0) {
                    const QModelIndex idx =
                        m_treeModel->indexForSession(m_currentSessionId);
                    if (idx.isValid()) {
                        QSignalBlocker blocker(
                            m_projectTree->selectionModel());
                        m_projectTree->setCurrentIndex(idx);
                    }
                }
            });

    layout->addWidget(m_projectTree, /*stretch=*/1);

    auto* midToolbar = new QToolBar(pane);
    midToolbar->setObjectName(QStringLiteral("leftPaneMidToolbar"));
    midToolbar->setFixedHeight(0);
    layout->addWidget(midToolbar);

    QIcon gearIcon;
    {
        QFile f(QStringLiteral(":/icons/gear.svg"));
        if (f.open(QIODevice::ReadOnly)) {
            QByteArray svg = f.readAll();
            const QColor textColor =
                palette().color(QPalette::ButtonText);
            svg.replace("currentColor",
                        textColor.name().toUtf8());
            QPixmap pm;
            pm.loadFromData(svg, "SVG");
            gearIcon = QIcon(pm);
        }
    }
    auto* settingsButton = new QPushButton(
        gearIcon, tr("Settings…"), pane);
    settingsButton->setObjectName(QStringLiteral("leftPaneSettingsButton"));
    connect(settingsButton, &QPushButton::clicked,
            this, &MainWindow::openSettingsDialog);
    layout->addWidget(settingsButton);

    return pane;
}

QWidget* MainWindow::buildRightColumn() {
    auto* column = new QWidget(this);
    column->setObjectName(QStringLiteral("rightColumn"));
    column->setMinimumWidth(440);

    auto* layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_buttonBar = new QToolBar(column);
    m_buttonBar->setObjectName(QStringLiteral("rightColumnButtonBar"));
    static_cast<QToolBar*>(m_buttonBar)->setFixedHeight(32);
    layout->addWidget(m_buttonBar);

    // §3.3: messageChatSplitter is the outer vertical splitter.
    // Top child = topSplitter (message + editor, horizontal).
    // Bottom child = chatInputWidget spanning the full width.
    m_messageChatSplitter =
        new RightClickSplitter(Qt::Vertical, column);
    m_messageChatSplitter->setObjectName(
        QStringLiteral("messageChatSplitter"));
    m_messageChatSplitter->setChildrenCollapsible(false);
    m_messageChatSplitter->setHandleWidth(5);

    m_topSplitter = new RightClickSplitter(
        Qt::Horizontal, m_messageChatSplitter);
    m_topSplitter->setObjectName(QStringLiteral("topSplitter"));
    m_topSplitter->setChildrenCollapsible(false);
    m_topSplitter->setHandleWidth(5);

    m_messageWindow = new MessageWindowWidget(m_topSplitter);
    m_messageWindow->setMinimumHeight(120);

    auto* editorPane = new EditorPaneWidget(m_topSplitter);
    editorPane->setMinimumHeight(120);
    editorPane->hide();
    m_editorPane = editorPane;

    connect(editorPane, &EditorPaneWidget::tabsChanged, this, [this]() {
        auto* ep = editorPaneWidget();
        if (ep && ep->tabCount() == 0 && m_editorPane->isVisible()) {
            m_suppressSplitterWrites = true;
            m_editorPane->hide();
            m_topSplitter->setSizes({1, 0});
            m_suppressSplitterWrites = false;
        }
    });

    m_topSplitter->addWidget(m_messageWindow);
    m_topSplitter->addWidget(editorPane);
    m_topSplitter->setStretchFactor(0, 1);
    m_topSplitter->setStretchFactor(1, 1);

    connect(m_topSplitter, &QSplitter::splitterMoved, this,
            &MainWindow::onTopSplitterMoved);
    connect(m_topSplitter, &RightClickSplitter::handleRightClicked,
            this, &MainWindow::resetTopMessageRatio);

    m_chatInputWidget = new ChatInputWidget(m_messageChatSplitter);
    m_chatInputWidget->setMinimumHeight(48);

    m_messageChatSplitter->addWidget(m_topSplitter);
    m_messageChatSplitter->addWidget(m_chatInputWidget);
    m_messageChatSplitter->setStretchFactor(0, 1);
    m_messageChatSplitter->setStretchFactor(1, 0);

    connect(m_messageChatSplitter, &QSplitter::splitterMoved, this,
            &MainWindow::onMessageChatSplitterMoved);
    connect(m_messageChatSplitter,
            &RightClickSplitter::handleRightClicked, this,
            &MainWindow::onMessageChatSplitterRightClicked);

    if (auto* doc = m_chatInputWidget->document()) {
        connect(doc, &QTextDocument::contentsChanged, this,
                &MainWindow::recomputeChatSplitAuto);
    }
    connect(m_chatInputWidget, &ChatInputWidget::chatStatusMessage,
            this, [this](const QString& msg) {
                if (statusBar()) statusBar()->showMessage(msg, 5000);
            });

    m_messagePane = m_topSplitter;

    layout->addWidget(m_messageChatSplitter, /*stretch=*/1);

    return column;
}

// --- Menu / status / shortcuts ----------------------------------

void MainWindow::buildMenuBar() {
    // Every menu action is created via the ActionRegistry singleton,
    // not via direct `new QAction` or `menu->addAction(text)`. The
    // registry owns the QAction lifetimes; this builder only
    // consumes them. A future KeyBindingsDialog will read the
    // registry directly without touching this function.
    ActionRegistry& reg = ActionRegistry::instance();

    auto ensure = [&](const QString& id, const QString& text,
                      const QKeySequence& shortcut = {},
                      const QString& statusTip = {}) -> QAction* {
        if (reg.contains(id)) {
            return reg.action(id);
        }
        return reg.registerAction(id, text, shortcut, statusTip);
    };

    auto* mb = menuBar();

    // --- File menu ---
    auto* fileMenu = mb->addMenu(tr("&File"));

    auto* newWindowAction = ensure(
        QStringLiteral("file.new_window"), tr("New &Window"),
        QKeySequence(QStringLiteral("Ctrl+Shift+N")));
    fileMenu->addAction(newWindowAction);
    connect(newWindowAction, &QAction::triggered, this,
            &MainWindow::onNewWindow, Qt::UniqueConnection);

    auto* newProjectAction = ensure(
        QStringLiteral("file.new_project"), tr("New &Project…"),
        QKeySequence(QStringLiteral("Ctrl+Shift+P")));
    fileMenu->addAction(newProjectAction);
    connect(newProjectAction, &QAction::triggered, this,
            &MainWindow::onNewProject, Qt::UniqueConnection);

    auto* newSessionAction = ensure(
        QStringLiteral("file.new_session"), tr("New &Session"),
        QKeySequence::New);
    fileMenu->addAction(newSessionAction);
    connect(newSessionAction, &QAction::triggered, this,
            &MainWindow::onNewSession, Qt::UniqueConnection);

    fileMenu->addSeparator();

    auto* openAction = ensure(
        QStringLiteral("file.open_file"), tr("&Open File…"),
        QKeySequence::Open);
    fileMenu->addAction(openAction);
    connect(openAction, &QAction::triggered, this,
            &MainWindow::openFileDialog, Qt::UniqueConnection);

    auto* saveAction = ensure(
        QStringLiteral("file.save"), tr("&Save"),
        QKeySequence::Save);
    fileMenu->addAction(saveAction);
    connect(saveAction, &QAction::triggered, this,
            &MainWindow::saveCurrentFile, Qt::UniqueConnection);

    auto* saveAsAction = ensure(
        QStringLiteral("file.save_as"), tr("Save &As…"),
        QKeySequence::SaveAs);
    fileMenu->addAction(saveAsAction);
    connect(saveAsAction, &QAction::triggered, this,
            &MainWindow::saveCurrentFileAs, Qt::UniqueConnection);

    fileMenu->addSeparator();

    auto* closeSessionAction = ensure(
        QStringLiteral("file.close_session"), tr("&Close Session"));
    fileMenu->addAction(closeSessionAction);

    fileMenu->addSeparator();

    auto* quitAction = ensure(
        QStringLiteral("file.quit"), tr("&Quit"),
        QKeySequence::Quit);
    fileMenu->addAction(quitAction);
    // QApplication::quit is a static slot, so UniqueConnection
    // isn't available here (Qt requires a member pointer). The
    // registry ensures the action is created once; a second
    // buildMenuBar on a reconstructed MainWindow would attach a
    // redundant connection, but quit is idempotent.
    connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);

    // --- Edit menu ---
    auto* editMenu = mb->addMenu(tr("&Edit"));
    editMenu->addAction(ensure(
        QStringLiteral("edit.undo"), tr("&Undo"), QKeySequence::Undo));
    editMenu->addAction(ensure(
        QStringLiteral("edit.redo"), tr("&Redo"), QKeySequence::Redo));
    editMenu->addSeparator();
    editMenu->addAction(ensure(
        QStringLiteral("edit.cut"), tr("Cu&t"), QKeySequence::Cut));
    editMenu->addAction(ensure(
        QStringLiteral("edit.copy"), tr("&Copy"), QKeySequence::Copy));
    editMenu->addAction(ensure(
        QStringLiteral("edit.paste"), tr("&Paste"), QKeySequence::Paste));
    editMenu->addSeparator();

    auto* preferencesAction = ensure(
        QStringLiteral("edit.preferences"), tr("&Preferences…"),
        QKeySequence::Preferences);
    editMenu->addAction(preferencesAction);
    connect(preferencesAction, &QAction::triggered,
            this, &MainWindow::openSettingsDialog, Qt::UniqueConnection);

    // --- View menu ---
    auto* viewMenu = mb->addMenu(tr("&View"));
    auto* toggleEditorAction = ensure(
        QStringLiteral("view.toggle_editor"), tr("Toggle &Editor"),
        QKeySequence(QStringLiteral("Ctrl+E")));
    viewMenu->addAction(toggleEditorAction);
    connect(toggleEditorAction, &QAction::triggered, this,
            &MainWindow::toggleEditor, Qt::UniqueConnection);

    auto* toggleLeftPaneAction = ensure(
        QStringLiteral("view.toggle_left_pane"), tr("Toggle &Left Pane"),
        QKeySequence(QStringLiteral("Ctrl+L")));
    viewMenu->addAction(toggleLeftPaneAction);
    connect(toggleLeftPaneAction, &QAction::triggered, this,
            &MainWindow::toggleLeftPane, Qt::UniqueConnection);

    viewMenu->addSeparator();

    auto* resetLayoutAction = ensure(
        QStringLiteral("view.reset_window_layout"),
        tr("&Reset Window Layout…"));
    viewMenu->addAction(resetLayoutAction);
    connect(resetLayoutAction, &QAction::triggered, this,
            &MainWindow::resetWindowLayout, Qt::UniqueConnection);

    // --- Session menu ---
    auto* sessionMenu = mb->addMenu(tr("&Session"));
    auto* renameSessionAction = ensure(
        QStringLiteral("session.rename"), tr("&Rename Session"));
    sessionMenu->addAction(renameSessionAction);
    connect(renameSessionAction, &QAction::triggered, this,
            &MainWindow::onRenameSelected, Qt::UniqueConnection);

    auto* changeCwdAction = ensure(
        QStringLiteral("session.change_cwd"),
        tr("Change &Working Directory…"));
    sessionMenu->addAction(changeCwdAction);
    connect(changeCwdAction, &QAction::triggered, this,
            &MainWindow::onChangeSessionCwd, Qt::UniqueConnection);

    auto* moveToProjectAction = ensure(
        QStringLiteral("session.move_to_project"),
        tr("&Move to Project…"));
    sessionMenu->addAction(moveToProjectAction);
    connect(moveToProjectAction, &QAction::triggered, this,
            &MainWindow::onMoveSessionToProject, Qt::UniqueConnection);

    sessionMenu->addSeparator();

    auto* deleteSessionAction = ensure(
        QStringLiteral("session.delete"), tr("&Delete Session"));
    sessionMenu->addAction(deleteSessionAction);
    connect(deleteSessionAction, &QAction::triggered, this,
            &MainWindow::onDeleteSelected, Qt::UniqueConnection);

    // --- Help menu ---
    auto* helpMenu = mb->addMenu(tr("&Help"));
    auto* aboutAction = ensure(
        QStringLiteral("help.about"),
        tr("&About %1…").arg(QApplication::applicationName()));
    helpMenu->addAction(aboutAction);
    // Lambda slots can't use Qt::UniqueConnection — Qt drops the
    // connection and warns on stderr. buildMenuBar() runs once per
    // window so duplicate wiring isn't a concern.
    connect(aboutAction, &QAction::triggered, this, [this]() {
        SettingsDialog dlg(this);
        dlg.setCurrentTab(2);
        dlg.exec();
    });
    auto* githubAction = ensure(
        QStringLiteral("help.github"), tr("&GitHub Repository"));
    helpMenu->addAction(githubAction);
    connect(githubAction, &QAction::triggered, this, []() {
        QDesktopServices::openUrl(
            QUrl(QString::fromLatin1(APP_HOMEPAGE)));
    });
}

void MainWindow::buildStatusBar() {
    auto* sb = statusBar();
    // Fusion's QSizeGrip paints an unstyled dotted hatch in the
    // corner that flickers against our dark QSS — and the main
    // window frame already provides resize grips everywhere.
    sb->setSizeGripEnabled(false);

    m_statusCwdLabel = new QLabel(tr("(no session)"), sb);
    m_statusCwdLabel->setObjectName(QStringLiteral("statusCwdLabel"));
    sb->addWidget(m_statusCwdLabel, /*stretch=*/1);

    m_statusSessionIdLabel = new QLabel(sb);
    m_statusSessionIdLabel->setObjectName(
        QStringLiteral("statusSessionIdLabel"));
    m_statusSessionIdLabel->hide();
    sb->addWidget(m_statusSessionIdLabel);
}

void MainWindow::installShortcuts() {
    auto* editorShortcut = new QShortcut(
        QKeySequence(QStringLiteral("Ctrl+E")), this);
    editorShortcut->setContext(Qt::ApplicationShortcut);
    connect(editorShortcut, &QShortcut::activated, this,
            &MainWindow::toggleEditor);

    auto* leftPaneShortcut = new QShortcut(
        QKeySequence(QStringLiteral("Ctrl+L")), this);
    leftPaneShortcut->setContext(Qt::ApplicationShortcut);
    connect(leftPaneShortcut, &QShortcut::activated, this,
            &MainWindow::toggleLeftPane);
}

void MainWindow::installGeometryTimers() {
    m_windowGeometryTimer = new QTimer(this);
    m_windowGeometryTimer->setSingleShot(true);
    m_windowGeometryTimer->setInterval(kGeometryDebounceMs);
    connect(m_windowGeometryTimer, &QTimer::timeout, this,
            &MainWindow::saveWindowGeometryNow);

    m_windowPositionTimer = new QTimer(this);
    m_windowPositionTimer->setSingleShot(true);
    m_windowPositionTimer->setInterval(kGeometryDebounceMs);
    connect(m_windowPositionTimer, &QTimer::timeout, this,
            &MainWindow::saveWindowPositionNow);

    m_leftPaneWidthTimer = new QTimer(this);
    m_leftPaneWidthTimer->setSingleShot(true);
    m_leftPaneWidthTimer->setInterval(kGeometryDebounceMs);
    connect(m_leftPaneWidthTimer, &QTimer::timeout, this,
            &MainWindow::saveLeftPaneWidthNow);

    m_topSplitterRatioTimer = new QTimer(this);
    m_topSplitterRatioTimer->setSingleShot(true);
    m_topSplitterRatioTimer->setInterval(kGeometryDebounceMs);
    connect(m_topSplitterRatioTimer, &QTimer::timeout, this,
            &MainWindow::saveTopSplitterRatioNow);

    // Coalesces expand/collapse clicks into a single
    // CSV write of `tree.expanded_ids` per §10.4.
    m_expandedIdsTimer = new QTimer(this);
    m_expandedIdsTimer->setSingleShot(true);
    m_expandedIdsTimer->setInterval(kGeometryDebounceMs);
    connect(m_expandedIdsTimer, &QTimer::timeout, this,
            &MainWindow::saveExpandedIdsNow);

    // Coalesces rapid chat splitter drags into a single
    // settings_kv write of `chat_splitter.*`. Right-click and
    // resize bypass this timer (immediate write per §3.4).
    m_chatSplitterWriteTimer = new QTimer(this);
    m_chatSplitterWriteTimer->setSingleShot(true);
    m_chatSplitterWriteTimer->setInterval(kGeometryDebounceMs);
    connect(m_chatSplitterWriteTimer, &QTimer::timeout, this,
            &MainWindow::saveChatSplitterStateNow);

    const QByteArray dwellRaw = Persistence::instance().getSetting(
        QStringLiteral("session.activation_dwell_ms"));
    int dwellMs = kDefaultActivationDwellMs;
    if (!dwellRaw.isEmpty()) {
        bool ok = false;
        const int v = QString::fromUtf8(dwellRaw).toInt(&ok);
        if (ok && v >= 0) dwellMs = v;
    }
    m_activationDwellTimer = new QTimer(this);
    m_activationDwellTimer->setSingleShot(true);
    m_activationDwellTimer->setInterval(dwellMs);
    connect(m_activationDwellTimer, &QTimer::timeout, this,
            [this]() {
                if (m_pendingDwellSessionId <= 0) return;
                try {
                    Persistence::instance().activateSession(
                        m_pendingDwellSessionId);
                } catch (const std::exception&) {
                }
            });
}

// --- Toggle slots -----------------------------------------------

void MainWindow::toggleEditor() {
    if (!m_editorPane || !m_topSplitter) return;
    const bool becomingVisible = !m_editorPane->isVisible();
    m_suppressSplitterWrites = true;
    m_editorPane->setVisible(becomingVisible);
    if (becomingVisible) {
        applyTopSplitterRatio();
    } else {
        m_topSplitter->setSizes({1, 0});
    }
    m_suppressSplitterWrites = false;
    writeBool(kKeyEditorVisible, becomingVisible);

    if (auto* pane = editorPaneWidget()) {
        if (becomingVisible) {
            if (pane->tabCount() == 0) {
                pane->newUntitled();
            }
        } else {
            pane->closePristineUntitledTabs();
        }
    }
}

void MainWindow::toggleLeftPane() {
    if (!m_leftPane || !m_outerSplitter) return;
    if (m_leftPane->isVisible()) {
        const auto sizes = m_outerSplitter->sizes();
        if (!sizes.isEmpty() && sizes.first() > 0) {
            m_savedLeftPaneWidth = sizes.first();
        }
        m_leftPane->hide();
        writeBool(kKeyLeftPaneVisible, false);
    } else {
        m_suppressSplitterWrites = true;
        m_leftPane->show();
        const int rightWidth =
            m_outerSplitter->width() - m_savedLeftPaneWidth - 5;
        m_outerSplitter->setSizes(
            {m_savedLeftPaneWidth, rightWidth > 0 ? rightWidth : 1});
        m_suppressSplitterWrites = false;
        writeBool(kKeyLeftPaneVisible, true);
    }
}

// --- Right-click reset slots ------------------------------------

void MainWindow::resetLeftPaneWidth() {
    if (!m_outerSplitter) return;
    m_savedLeftPaneWidth = kDefaultLeftPaneWidth;
    m_suppressSplitterWrites = true;
    const int rightWidth =
        m_outerSplitter->width() - kDefaultLeftPaneWidth - 5;
    m_outerSplitter->setSizes(
        {kDefaultLeftPaneWidth, rightWidth > 0 ? rightWidth : 1});
    m_suppressSplitterWrites = false;
    // §3.2 requires the reset to overwrite the persisted value
    // immediately so the gesture survives relaunch even if the
    // user closes the window before the debounce interval.
    writeInt(kKeyLeftPaneWidth, kDefaultLeftPaneWidth);
}

void MainWindow::applyTopSplitterRatio(double ratio) {
    if (!m_topSplitter) return;
    if (ratio < 0.0)
        ratio = readDouble(kKeyTopSplitterRatio, kDefaultTopMessageRatio);
    writeDouble(kKeyTopSplitterRatio, ratio);
    const bool prev = m_suppressSplitterWrites;
    m_suppressSplitterWrites = true;
    const int total = m_topSplitter->width();
    if (total > 0) {
        const int left = int(total * ratio);
        m_topSplitter->setSizes({left, total - left});
    } else {
        m_topSplitter->setSizes({1, 1});
    }
    m_suppressSplitterWrites = prev;
}

void MainWindow::resetTopMessageRatio() {
    applyTopSplitterRatio(kDefaultTopMessageRatio);
}

// --- Editor accessors / file I/O --------------------------------

EditorPaneWidget* MainWindow::editorPaneWidget() const {
    return qobject_cast<EditorPaneWidget*>(m_editorPane);
}

CodeEditor* MainWindow::openFile(const QString& path) {
    auto* pane = editorPaneWidget();
    if (!pane) return nullptr;

    // Expand the editor pane if the user had it collapsed —
    // opening a file with the editor hidden would be useless
    // feedback per §6.3.
    if (!pane->isVisible()) {
        toggleEditor();
    }
    return pane->openFile(path);
}

void MainWindow::openFileDialog() {
    // There's no active session context yet, so we root the dialog
    // at the process cwd. When per-session cwd lands the second
    // arg becomes `session->cwd()`.
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open File"), QDir::currentPath());
    if (path.isEmpty()) return;
    openFile(path);
}

void MainWindow::saveCurrentFile() {
    auto* pane = editorPaneWidget();
    if (!pane) return;
    auto* ed = pane->currentEditor();
    if (!ed) return;
    if (ed->filePath().isEmpty()) {
        saveCurrentFileAs();
        return;
    }
    pane->saveCurrent();
}

void MainWindow::saveCurrentFileAs() {
    auto* pane = editorPaneWidget();
    if (!pane) return;
    auto* ed = pane->currentEditor();
    if (!ed) return;
    const QString start = ed->filePath().isEmpty()
                              ? QDir::currentPath()
                              : QFileInfo(ed->filePath()).absolutePath();
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save File As"), start);
    if (path.isEmpty()) return;
    pane->saveAsCurrent(path);
}

void MainWindow::openSettingsDialog() {
    SettingsDialog dlg(this);
    dlg.exec();
}

// --- Reset window layout ----------------------------------------

void MainWindow::resetWindowLayout() {
    ThemedQtDialog dlg(this);
    dlg.setWindowTitle(tr("Reset Window Layout"));
    auto* layout = new QVBoxLayout(&dlg);
    auto* msg = new QLabel(
        tr("This will reset window size, splitter positions, "
           "left pane visibility and width, editor visibility, "
           "and chat input rows to their defaults.\n\n"
           "The session tree, open buffers, chat history, and "
           "other preferences are not affected.\n\n"
           "Continue?"),
        &dlg);
    msg->setWordWrap(true);
    layout->addWidget(msg);

    auto* box = dlg.buildButtonBox(QDialogButtonBox::Ok |
                                   QDialogButtonBox::Cancel);
    if (auto* okBtn = box->button(QDialogButtonBox::Ok)) {
        dlg.setAccentButton(okBtn);
        okBtn->setText(tr("Reset"));
    }
    layout->addWidget(box);

    if (dlg.exec() != QDialog::Accepted) return;

    for (const QString& key : kGeometryKeys) {
        clearKey(key);
    }
    applyDefaultGeometry();
}

void MainWindow::applyDefaultGeometry() {
    m_suppressSplitterWrites = true;

    resize(kDefaultWidth, kDefaultHeight);
    if (isMaximized()) showNormal();

    // Left pane visible at default width.
    if (m_leftPane) m_leftPane->show();
    m_savedLeftPaneWidth = kDefaultLeftPaneWidth;
    if (m_outerSplitter) {
        const int rightWidth =
            m_outerSplitter->width() - kDefaultLeftPaneWidth - 5;
        m_outerSplitter->setSizes(
            {kDefaultLeftPaneWidth, rightWidth > 0 ? rightWidth : 1});
    }

    // Editor hidden, top split collapsed to all-message.
    if (m_editorPane) m_editorPane->hide();
    if (m_topSplitter) m_topSplitter->setSizes({1, 0});

    // Chat splitter back to auto mode.
    m_chatSplitUserOverridden = false;
    m_chatSplitStashedManualSize = -1;
    m_suppressChatSplitterSignal = true;
    recomputeChatSplitAuto();
    m_suppressChatSplitterSignal = false;

    m_suppressSplitterWrites = false;
}

// --- Load path --------------------------------------------------

void MainWindow::loadGeometryFromSettings() {
    m_suppressSplitterWrites = true;

    const int w = readInt(kKeyWindowWidth, kDefaultWidth);
    const int h = readInt(kKeyWindowHeight, kDefaultHeight);
    resize(w, h);

    const int x = readInt(kKeyWindowX, -1);
    const int y = readInt(kKeyWindowY, -1);
    if (x >= 0 && y >= 0) {
        move(x, y);
    }

    if (readBool(kKeyWindowMaximized, false)) {
        showMaximized();
    }

    const int lpw = readInt(kKeyLeftPaneWidth, kDefaultLeftPaneWidth);
    m_savedLeftPaneWidth = lpw > 0 ? lpw : kDefaultLeftPaneWidth;

    const bool leftVisible = readBool(kKeyLeftPaneVisible, true);
    if (m_leftPane) m_leftPane->setVisible(leftVisible);
    if (m_outerSplitter) {
        if (leftVisible) {
            const int rightWidth =
                m_outerSplitter->width() - m_savedLeftPaneWidth - 5;
            m_outerSplitter->setSizes(
                {m_savedLeftPaneWidth, rightWidth > 0 ? rightWidth : 1});
        } else {
            m_outerSplitter->setSizes({0, m_outerSplitter->width()});
        }
    }

    const bool editorVisible = readBool(kKeyEditorVisible, false);
    if (m_editorPane) m_editorPane->setVisible(editorVisible);
    if (m_topSplitter) {
        if (!editorVisible) {
            m_topSplitter->setSizes({1, 0});
        }
        m_topSplitterNeedsApply = editorVisible;
    }

    // Stash the raw persisted chat splitter state for
    // validateAndApplyChatSplitterState() to consume after the
    // first layout pass. We can't clamp here because
    // m_messageChatSplitter->height() is still zero.
    m_chatSplitPendingManualSize =
        readInt(kKeyChatSplitterManualSize, -1);
    m_chatSplitPendingStashedManualSize =
        readInt(kKeyChatSplitterStashedManualSize, -1);
    m_chatSplitNeedsValidation = true;

    m_suppressSplitterWrites = false;
}

// --- Save-now slots ---------------------------------------------

void MainWindow::saveWindowGeometryNow() {
    if (isMaximized()) {
        writeBool(kKeyWindowMaximized, true);
        // Don't overwrite width/height while maximized — we want
        // to preserve the last non-maximized size for the restore
        // path after the user un-maximizes.
        return;
    }
    writeBool(kKeyWindowMaximized, false);
    writeInt(kKeyWindowWidth, width());
    writeInt(kKeyWindowHeight, height());
}

void MainWindow::saveWindowPositionNow() {
    if (isMaximized()) return;
    writeInt(kKeyWindowX, x());
    writeInt(kKeyWindowY, y());
}

void MainWindow::saveLeftPaneWidthNow() {
    if (!m_outerSplitter) return;
    const auto sizes = m_outerSplitter->sizes();
    if (sizes.isEmpty()) return;
    const int w = sizes.first();
    if (w <= 0) return;  // Collapsed; skip to preserve prior width.
    m_savedLeftPaneWidth = w;
    writeInt(kKeyLeftPaneWidth, w);
}

void MainWindow::saveTopSplitterRatioNow() {
    if (!m_topSplitter) return;
    if (!m_editorPane || !m_editorPane->isVisible()) return;
    const auto sizes = m_topSplitter->sizes();
    if (sizes.size() != 2) return;
    const int total = sizes[0] + sizes[1];
    if (total <= 0) return;
    applyTopSplitterRatio(double(sizes[0]) / double(total));
}

// --- Splitter drag hooks ----------------------------------------

void MainWindow::onOuterSplitterMoved() {
    if (m_suppressSplitterWrites) return;
    if (m_leftPaneWidthTimer) m_leftPaneWidthTimer->start();
}

void MainWindow::onTopSplitterMoved() {
    if (m_suppressSplitterWrites) return;
    if (m_topSplitterRatioTimer) m_topSplitterRatioTimer->start();
}

// --- Event overrides --------------------------------------------

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    if (m_windowGeometryTimer) m_windowGeometryTimer->start();
    // §7.2 + §3.4: window resize resets both the manual override
    // and the stash to empty — the absolute pixel position the
    // user picked almost never survives a big layout change, so
    // dropping both is less surprising than trying to preserve
    // them. The write is immediate per the §3.4 event table.
    const bool hadChatState =
        m_chatSplitUserOverridden || m_chatSplitStashedManualSize >= 0;
    m_chatSplitUserOverridden = false;
    m_chatSplitStashedManualSize = -1;
    recomputeChatSplitAuto();
    if (hadChatState) {
        saveChatSplitterStateNow();
    }
}

void MainWindow::moveEvent(QMoveEvent* event) {
    QMainWindow::moveEvent(event);
    if (m_windowPositionTimer) m_windowPositionTimer->start();
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    // Sub-minimum persisted values (e.g., 200×200 from a hand-
    // edit) are clamped up to setMinimumSize by Qt on the first
    // layout pass; that final frame size is what the user sees.
    // Rewrite immediately so the DB reflects the clamped value
    // instead of the stale bad one. Skip the debounce: we're
    // correcting a load-time error, not rate-limiting a user
    // gesture.
    if (width() > kMinWidth && height() > kMinHeight) return;
    saveWindowGeometryNow();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    flushPendingGeometryWrites();
    QMainWindow::closeEvent(event);
}

// --- Chat shell -------------------------------------------------

void MainWindow::ensureScratchSessionExists() {
    auto& db = Persistence::instance();
    QList<SessionRow> sessions = db.listSessions();

    // Prefer `session.last_active_id` from settings_kv when it
    // still resolves to an existing row; otherwise fall back to
    // the first session, and only bootstrap "Scratch" if the DB
    // is empty. Tree-driven new-session dialogs replace the
    // fallback path.
    qint64 chosen = 0;
    const qint64 lastActive =
        static_cast<qint64>(readInt(kKeySessionLastActiveId, 0));
    if (lastActive > 0) {
        for (const auto& s : sessions) {
            if (s.id == lastActive) {
                chosen = s.id;
                break;
            }
        }
    }
    if (chosen == 0) {
        if (sessions.isEmpty()) {
            const QString title =
                tr("Untitled %1")
                    .arg(QDateTime::currentDateTime().toString(
                        QStringLiteral("yyyy-MM-dd hh:mm")));
            chosen = db.createSession(std::nullopt, title,
                                      QDir::currentPath());
        } else {
            chosen = sessions.first().id;
        }
    }

    setActiveSession(chosen);
    restoreExpandedIds();
}

// --- Tree wiring ------------------------------------------------

void MainWindow::setActiveSession(qint64 sessionId) {
    if (sessionId <= 0) return;
    m_currentSessionId = sessionId;

    if (m_messageWindow)
        m_messageWindow->setCurrentSessionId(sessionId);
    if (m_chatInputWidget)
        m_chatInputWidget->setCurrentSessionId(sessionId);
    if (auto* ep = editorPaneWidget()) {
        ep->setCurrentSessionId(sessionId);
        const bool hasBuffers = ep->tabCount() > 0;
        m_suppressSplitterWrites = true;
        m_editorPane->setVisible(hasBuffers);
        if (hasBuffers && !m_topSplitterNeedsApply) {
            applyTopSplitterRatio();
        } else if (!hasBuffers) {
            m_topSplitter->setSizes({1, 0});
        }
        m_suppressSplitterWrites = false;
    }

    {
        QString sessionName;
        QString cwdForLabel;
        for (const auto& s : Persistence::instance().listSessions()) {
            if (s.id == sessionId) {
                sessionName = s.title;
                cwdForLabel = s.cwd;
                break;
            }
        }

        if (m_statusSessionIdLabel) {
            m_statusSessionIdLabel->setText(sessionName);
            m_statusSessionIdLabel->show();
        }

        if (m_statusCwdLabel) {
            m_statusCwdLabel->setText(
                cwdForLabel.isEmpty() ? tr("(no session)") : cwdForLabel);
        }
    }

    // Defer the activateSession() call (which bumps last_used and
    // triggers sort reorder) by a configurable dwell time so fast
    // click-through navigation doesn't reorder the tree under the
    // cursor.
    m_pendingDwellSessionId = sessionId;
    if (m_activationDwellTimer) {
        m_activationDwellTimer->start();
    } else {
        try {
            Persistence::instance().activateSession(sessionId);
        } catch (const std::exception&) {
        }
    }

    writeInt(kKeySessionLastActiveId, int(sessionId));

    // Sync the tree selection with the active session so external
    // callers (last_active_id restore, chat-shell routing) end up
    // visually consistent with a click-driven selection. Guard
    // re-entry since setCurrentIndex fires selectionChanged again.
    if (m_projectTree && m_treeModel) {
        const QModelIndex idx = m_treeModel->indexForSession(sessionId);
        if (idx.isValid() &&
            m_projectTree->currentIndex() != idx) {
            QSignalBlocker blocker(m_projectTree->selectionModel());
            m_projectTree->setCurrentIndex(idx);
        }
    }
}

void MainWindow::onTreeSelectionChanged(
    const QItemSelection& selected, const QItemSelection& /*deselected*/) {
    const auto indexes = selected.indexes();
    for (const QModelIndex& idx : indexes) {
        if (!idx.isValid()) continue;
        const int kind =
            idx.data(ProjectSessionTreeModel::ItemTypeRole).toInt();
        if (kind != ProjectSessionTreeModel::Session) continue;
        const qint64 sid =
            idx.data(ProjectSessionTreeModel::ItemIdRole).toLongLong();
        if (sid > 0 && sid != m_currentSessionId) {
            setActiveSession(sid);
        }
        return;
    }
}

namespace {

// Walk every Project node under `parent` and call `visit(idx)`.
void walkProjectIndexes(
    const ProjectSessionTreeModel* model, const QModelIndex& parent,
    const std::function<void(const QModelIndex&)>& visit) {
    if (!model) return;
    const int n = model->rowCount(parent);
    for (int i = 0; i < n; ++i) {
        const QModelIndex idx = model->index(i, 0, parent);
        if (!idx.isValid()) continue;
        const int kind =
            idx.data(ProjectSessionTreeModel::ItemTypeRole).toInt();
        if (kind == ProjectSessionTreeModel::Project) {
            visit(idx);
            walkProjectIndexes(model, idx, visit);
        }
    }
}

}  // namespace

void MainWindow::saveExpandedIdsNow() {
    if (!m_projectTree || !m_treeModel) return;
    QStringList ids;
    walkProjectIndexes(
        m_treeModel, QModelIndex(),
        [&](const QModelIndex& idx) {
            if (!m_projectTree->isExpanded(idx)) return;
            const qint64 pid =
                idx.data(ProjectSessionTreeModel::ItemIdRole).toLongLong();
            if (pid > 0) ids.append(QString::number(pid));
        });
    Persistence::instance().setSetting(
        kKeyTreeExpandedIds, ids.join(QChar(',')).toUtf8());
}

void MainWindow::restoreExpandedIds() {
    if (!m_projectTree || !m_treeModel) return;
    // Arm the modelReset handler as soon as we begin the first
    // restore. Even an empty persisted value counts as "startup is
    // over" — a subsequent resort should then round-trip normally.
    m_treeExpandedIdsRestored = true;
    const QByteArray raw =
        Persistence::instance().getSetting(kKeyTreeExpandedIds);
    if (raw.isEmpty()) return;

    // CSV of project dbIds. Parse into a set for O(1) lookup
    // during the walk.
    const QStringList parts =
        QString::fromUtf8(raw).split(QChar(','), Qt::SkipEmptyParts);
    QSet<qint64> wanted;
    for (const QString& p : parts) {
        bool ok = false;
        const qint64 v = p.trimmed().toLongLong(&ok);
        if (ok && v > 0) wanted.insert(v);
    }
    if (wanted.isEmpty()) return;

    walkProjectIndexes(
        m_treeModel, QModelIndex(),
        [&](const QModelIndex& idx) {
            const qint64 pid =
                idx.data(ProjectSessionTreeModel::ItemIdRole).toLongLong();
            if (wanted.contains(pid)) m_projectTree->expand(idx);
        });
}

int MainWindow::chatSplitterMinTopHeight() const {
    return kChatSplitterMinTopPx;
}

int MainWindow::chatSplitterMinBottomHeight() const {
    if (!m_chatInputWidget) return 48;
    const int minRows = kDefaultChatSplitterMinBottomRows;
    const QFontMetrics fm(m_chatInputWidget->font());
    const int lineH = fm.lineSpacing();
    const int padding =
        int(m_chatInputWidget->contentsMargins().top()) +
        int(m_chatInputWidget->contentsMargins().bottom()) +
        2 * m_chatInputWidget->frameWidth();
    return minRows * lineH + padding;
}

int MainWindow::chatSplitterRowCap() const {
    if (!m_chatInputWidget) return 200;
    const int maxRows = readInt(
        QStringLiteral("chat_input.max_rows"), kDefaultChatInputMaxRows);
    if (maxRows <= 0) return INT_MAX;
    const QFontMetrics fm(m_chatInputWidget->font());
    const int lineH = fm.lineSpacing();
    const int padding =
        int(m_chatInputWidget->contentsMargins().top()) +
        int(m_chatInputWidget->contentsMargins().bottom()) +
        2 * m_chatInputWidget->frameWidth();
    return maxRows * lineH + padding;
}

void MainWindow::recomputeChatSplitAuto() {
    if (!m_messageChatSplitter || !m_chatInputWidget) return;
    if (m_chatSplitUserOverridden) return;

    const int splitterH = m_messageChatSplitter->height();
    if (splitterH <= 0) return;

    // Content height = document layout size plus the widget's
    // frame/margins. QPlainTextEdit exposes its document through
    // document()->documentLayout()->documentSize().
    int contentH = 0;
    if (auto* doc = m_chatInputWidget->document()) {
        const auto layout = doc->documentLayout();
        if (layout) {
            const QFontMetrics fm(m_chatInputWidget->font());
            contentH =
                int(layout->documentSize().height()) * fm.lineSpacing() +
                int(m_chatInputWidget->contentsMargins().top()) +
                int(m_chatInputWidget->contentsMargins().bottom()) +
                2 * m_chatInputWidget->frameWidth();
        }
    }

    const int minH = chatSplitterMinBottomHeight();
    const int rowCap = chatSplitterRowCap();
    const int halfCap = splitterH / 2;

    int desired = contentH;
    if (desired < minH) desired = minH;
    const int upper = std::min(rowCap, halfCap);
    if (desired > upper) desired = upper;
    if (desired < minH) desired = minH;  // halfCap may be < minH in tiny windows

    const int messageH = splitterH - desired;
    if (messageH <= 0) return;

    m_suppressChatSplitterSignal = true;
    m_messageChatSplitter->setSizes({messageH, desired});
    m_suppressChatSplitterSignal = false;
}

void MainWindow::saveChatSplitterStateNow() {
    // Canonical writer for both chat_splitter.* keys. Reads the
    // live runtime state rather than the event payload so drag /
    // right-click / resize / validation all serialize the same
    // bit of truth. -1 sentinels map to `clearSetting()` so a
    // fresh DB stays lean, matching the load-path defaults.
    if (m_chatSplitUserOverridden && m_messageChatSplitter) {
        const auto sizes = m_messageChatSplitter->sizes();
        if (sizes.size() == 2 && sizes[1] > 0) {
            writeInt(kKeyChatSplitterManualSize, sizes[1]);
        } else {
            clearKey(kKeyChatSplitterManualSize);
        }
    } else {
        clearKey(kKeyChatSplitterManualSize);
    }

    if (m_chatSplitStashedManualSize > 0) {
        writeInt(kKeyChatSplitterStashedManualSize,
                 m_chatSplitStashedManualSize);
    } else {
        clearKey(kKeyChatSplitterStashedManualSize);
    }
}

void MainWindow::validateAndApplyChatSplitterState() {
    // Runs once on the first layout pass after loadGeometryFromSettings
    // captured the raw persisted values into the m_chatSplitPending*
    // members. Clamps manual_size into the [minBottom, splitterH -
    // minTop] band (asymmetric with the auto-mode 50% cap: §3.4 DoD
    // lines 2989–3017 say manual survives above 50%), and drops
    // stashed_manual_size if it exceeds the full auto-mode cap.
    m_chatSplitNeedsValidation = false;

    if (!m_messageChatSplitter || !m_chatInputWidget) return;
    const int splitterH = m_messageChatSplitter->height();
    if (splitterH <= 0) {
        // Layout still not ready — defer another tick. Bail without
        // re-scheduling; the caller owns the singleShot cadence.
        return;
    }

    // --- manual_size: child-minimum band only -------------------
    int manual = m_chatSplitPendingManualSize;
    bool manualRewritten = false;
    if (manual >= 0) {
        const int minBottom = chatSplitterMinBottomHeight();
        const int minTop = chatSplitterMinTopHeight();
        const int maxBottom = splitterH - minTop;
        int clamped = manual;
        if (maxBottom < minBottom) {
            // Pathologically small window — both floors conflict.
            // Prefer the bottom floor since that's the one the user
            // is actively interacting with.
            clamped = minBottom;
        } else {
            if (clamped < minBottom) clamped = minBottom;
            if (clamped > maxBottom) clamped = maxBottom;
        }
        if (clamped != manual) {
            manualRewritten = true;
            manual = clamped;
        }

        m_chatSplitUserOverridden = true;
        const int topH = std::max(splitterH - manual, 1);
        m_suppressChatSplitterSignal = true;
        m_messageChatSplitter->setSizes({topH, manual});
        m_suppressChatSplitterSignal = false;

        if (manualRewritten) {
            writeInt(kKeyChatSplitterManualSize, manual);
        }
    } else {
        m_chatSplitUserOverridden = false;
    }

    // --- stashed_manual_size: full 50%/row-cap rule -------------
    int stashed = m_chatSplitPendingStashedManualSize;
    if (stashed >= 0) {
        const int halfCap = splitterH / 2;
        const int rowCap = chatSplitterRowCap();
        const int upper = std::min(halfCap, rowCap);
        const int minBottom = chatSplitterMinBottomHeight();
        if (stashed < minBottom || stashed > upper) {
            // Violates auto-mode cap → revert to empty stash.
            m_chatSplitStashedManualSize = -1;
            clearKey(kKeyChatSplitterStashedManualSize);
        } else {
            m_chatSplitStashedManualSize = stashed;
        }
    } else {
        m_chatSplitStashedManualSize = -1;
    }

    // Clear the one-shot pending carriers so a subsequent
    // validation pass (e.g., a second deferred tick) is a no-op.
    m_chatSplitPendingManualSize = -1;
    m_chatSplitPendingStashedManualSize = -1;

    if (!m_chatSplitUserOverridden) {
        recomputeChatSplitAuto();
    }
}

void MainWindow::onMessageChatSplitterMoved(int /*pos*/, int /*index*/) {
    if (m_suppressChatSplitterSignal) return;
    // Any explicit drag flips us into manual mode. Per §3.4 event
    // table, a drag clears the stash (stashed_manual_size = -1)
    // rather than refreshing it — the stash is specifically a
    // right-click artifact, not a drag one. A subsequent right-
    // click will create a new stash at the auto-mode snapshot.
    m_chatSplitUserOverridden = true;
    m_chatSplitStashedManualSize = -1;
    if (m_chatSplitterWriteTimer) m_chatSplitterWriteTimer->start();
}

void MainWindow::onMessageChatSplitterRightClicked() {
    if (!m_messageChatSplitter) return;
    const auto sizes = m_messageChatSplitter->sizes();
    if (sizes.size() != 2) return;
    const int splitterH = m_messageChatSplitter->height();
    if (splitterH <= 0) return;

    const int halfCap = splitterH / 2;

    if (m_chatSplitUserOverridden) {
        // Manual → auto. Per §7.2, this is a *conditional* flip:
        // if the current bottom size exceeds 50% of splitterH,
        // the toggle is a no-op because auto would shrink the
        // chat area — which is never what the gesture means.
        if (sizes[1] > halfCap) {
            return;
        }
        m_chatSplitStashedManualSize = sizes[1];
        m_chatSplitUserOverridden = false;
        recomputeChatSplitAuto();
        // §3.4: right-click is an immediate write of both keys.
        saveChatSplitterStateNow();
        return;
    }

    // Auto → manual. Restore stash if we have one; otherwise
    // this is a no-op (nothing to restore).
    if (m_chatSplitStashedManualSize <= 0) return;
    int inputH = m_chatSplitStashedManualSize;
    // The stash was validated at startup against the auto-mode
    // cap; belt-and-braces clamp it again against the current
    // splitterH in case the window has shrunk since the stash
    // was taken (e.g., user opened the editor pane and the
    // right column got narrower).
    const int minBottom = chatSplitterMinBottomHeight();
    if (inputH > splitterH - chatSplitterMinTopHeight()) {
        inputH = splitterH - chatSplitterMinTopHeight();
    }
    if (inputH < minBottom) inputH = minBottom;
    if (inputH <= 0) return;
    m_chatSplitUserOverridden = true;
    m_suppressChatSplitterSignal = true;
    m_messageChatSplitter->setSizes({splitterH - inputH, inputH});
    m_suppressChatSplitterSignal = false;
    saveChatSplitterStateNow();
}

// --- Tree CRUD --------------------------------------------------

QModelIndex MainWindow::selectedIndex() const {
    if (!m_projectTree) return {};
    const auto sel = m_projectTree->selectionModel();
    if (!sel) return {};
    const auto rows = sel->selectedRows();
    if (!rows.isEmpty()) return rows.first();
    return m_projectTree->currentIndex();
}

qint64 MainWindow::selectedProjectId() const {
    const QModelIndex idx = selectedIndex();
    if (!idx.isValid()) return 0;
    const int kind =
        idx.data(ProjectSessionTreeModel::ItemTypeRole).toInt();
    if (kind != ProjectSessionTreeModel::Project) return 0;
    return idx.data(ProjectSessionTreeModel::ItemIdRole).toLongLong();
}

qint64 MainWindow::selectedSessionId() const {
    const QModelIndex idx = selectedIndex();
    if (!idx.isValid()) return 0;
    const int kind =
        idx.data(ProjectSessionTreeModel::ItemTypeRole).toInt();
    if (kind != ProjectSessionTreeModel::Session) return 0;
    return idx.data(ProjectSessionTreeModel::ItemIdRole).toLongLong();
}

void MainWindow::onNewWindow() {
    QProcess::startDetached(
        QCoreApplication::applicationFilePath(), QStringList());
}

void MainWindow::onNewProject() {
    ThemedQtDialog dlg(this);
    dlg.setWindowTitle(tr("New Project"));
    auto* form = new QVBoxLayout(&dlg);

    form->addWidget(new QLabel(tr("Project name:"), &dlg));
    auto* nameEdit = new QLineEdit(&dlg);
    nameEdit->setObjectName(QStringLiteral("newProjectName"));
    form->addWidget(nameEdit);

    form->addWidget(new QLabel(tr("Root path:"), &dlg));
    auto* rowWidget = new QWidget(&dlg);
    auto* row = new QHBoxLayout(rowWidget);
    row->setContentsMargins(0, 0, 0, 0);
    auto* pathEdit = new QLineEdit(rowWidget);
    pathEdit->setObjectName(QStringLiteral("newProjectPath"));
    pathEdit->setText(QDir::homePath());
    auto* browseBtn = new QPushButton(tr("Browse…"), rowWidget);
    connect(browseBtn, &QPushButton::clicked, &dlg, [this, pathEdit]() {
        const QString p = QFileDialog::getExistingDirectory(
            this, tr("Select root path"), pathEdit->text());
        if (!p.isEmpty()) pathEdit->setText(p);
    });
    row->addWidget(pathEdit, /*stretch=*/1);
    row->addWidget(browseBtn);
    form->addWidget(rowWidget);

    form->addWidget(new QLabel(tr("Parent project:"), &dlg));
    auto* parentCombo = new QComboBox(&dlg);
    parentCombo->setObjectName(QStringLiteral("newProjectParentCombo"));
    parentCombo->addItem(tr("(top level)"),
                         QVariant::fromValue<qlonglong>(0));
    const auto existingProjects = Persistence::instance().listProjects();
    for (const auto& p : existingProjects) {
        parentCombo->addItem(p.name,
                             QVariant::fromValue<qlonglong>(p.id));
    }
    // Default the parent combo to the currently-selected project
    // (if any) — the common case is "create a sub-project of the
    // thing I just right-clicked".
    const qint64 preselect = selectedProjectId();
    if (preselect > 0) {
        const int idx = parentCombo->findData(
            QVariant::fromValue<qlonglong>(preselect));
        if (idx >= 0) parentCombo->setCurrentIndex(idx);
    }
    form->addWidget(parentCombo);

    auto* box = dlg.buildButtonBox(QDialogButtonBox::Ok |
                                   QDialogButtonBox::Cancel);
    if (auto* ok = box->button(QDialogButtonBox::Ok)) {
        dlg.setAccentButton(ok);
        ok->setText(tr("Create"));
    }
    form->addWidget(box);

    // Validate name non-empty; veto with shake otherwise.
    connect(&dlg, &ThemedQtDialog::aboutToAccept, &dlg, [&]() {
        if (nameEdit->text().trimmed().isEmpty()) {
            dlg.vetoAccept();
            dlg.shake();
        }
    });

    nameEdit->setFocus();
    if (dlg.exec() != QDialog::Accepted) return;

    const QString name = nameEdit->text().trimmed();
    const QString path = pathEdit->text().trimmed().isEmpty()
                             ? QDir::homePath()
                             : pathEdit->text().trimmed();
    const qlonglong parentId = parentCombo->currentData().toLongLong();
    std::optional<qint64> parentOpt;
    if (parentId > 0) parentOpt = parentId;

    try {
        Persistence::instance().createProject(parentOpt, name, path);
    } catch (const std::exception&) {
    }
}

void MainWindow::onNewSession() {
    // Use whichever project is selected as the session's home;
    // otherwise the new session becomes an orphan. Title defaults
    // to a timestamped placeholder per §5.2 row 5.
    std::optional<qint64> parentProject;
    if (const qint64 pid = selectedProjectId(); pid > 0) {
        parentProject = pid;
    } else if (const qint64 sid = selectedSessionId(); sid > 0) {
        // If a session is selected, fall back to its parent
        // project (or orphan) so the new row lands next to it.
        const auto sessions = Persistence::instance().listSessions();
        for (const auto& s : sessions) {
            if (s.id == sid) {
                parentProject = s.projectId;
                break;
            }
        }
    }

    const QString title = tr("Untitled %1")
                              .arg(QDateTime::currentDateTime().toString(
                                  QStringLiteral("yyyy-MM-dd hh:mm")));

    // Default cwd from the parent project's root_path when we
    // have one; otherwise fall back to $HOME so Claude Code CLI
    // has a real directory.
    QString cwd = QDir::homePath();
    if (parentProject.has_value()) {
        const auto projects = Persistence::instance().listProjects();
        for (const auto& p : projects) {
            if (p.id == *parentProject) {
                cwd = p.rootPath;
                break;
            }
        }
    }

    try {
        const qint64 sid = Persistence::instance().createSession(
            parentProject, title, cwd);
        setActiveSession(sid);
    } catch (const std::exception&) {
    }
}

void MainWindow::onRenameSelected() {
    const QModelIndex idx = selectedIndex();
    if (!idx.isValid() || !m_projectTree) return;
    m_projectTree->edit(idx);
}

void MainWindow::onDeleteSelected() {
    const QModelIndex idx = selectedIndex();
    if (!idx.isValid()) return;
    const int kind =
        idx.data(ProjectSessionTreeModel::ItemTypeRole).toInt();
    const qint64 dbId =
        idx.data(ProjectSessionTreeModel::ItemIdRole).toLongLong();
    const QString name = idx.data(Qt::DisplayRole).toString();

    ThemedQtDialog dlg(this);
    auto* lay = new QVBoxLayout(&dlg);
    QString text;
    if (kind == ProjectSessionTreeModel::Project) {
        dlg.setWindowTitle(tr("Delete Project"));
        text = tr("Delete project '%1'?\n\n"
                  "Its sub-projects will be promoted to top-level "
                  "and its sessions will become orphans.")
                   .arg(name);
    } else {
        dlg.setWindowTitle(tr("Delete Session"));
        const auto msgs = Persistence::instance().listMessagesForSession(dbId);
        text = tr("Delete session '%1'?\n\n"
                  "%n message(s) will be lost.", "", int(msgs.size()))
                   .arg(name);
    }
    auto* msgLabel = new QLabel(text, &dlg);
    msgLabel->setWordWrap(true);
    lay->addWidget(msgLabel);

    auto* box = dlg.buildButtonBox(QDialogButtonBox::Ok |
                                   QDialogButtonBox::Cancel);
    if (auto* ok = box->button(QDialogButtonBox::Ok)) {
        dlg.setDestructiveButton(ok);
        ok->setText(tr("Delete"));
    }
    lay->addWidget(box);

    if (dlg.exec() != QDialog::Accepted) return;

    try {
        if (kind == ProjectSessionTreeModel::Project) {
            Persistence::instance().deleteProject(dbId);
        } else {
            const bool wasCurrent = (dbId == m_currentSessionId);
            Persistence::instance().deleteSession(dbId);
            if (wasCurrent) {
                // Fall back to whatever session now sits at the top
                // of the list; if none, create a fresh Scratch.
                const auto remaining =
                    Persistence::instance().listSessions();
                if (!remaining.isEmpty()) {
                    setActiveSession(remaining.first().id);
                } else {
                    const QString fallbackTitle =
                        tr("Untitled %1")
                            .arg(QDateTime::currentDateTime().toString(
                                QStringLiteral("yyyy-MM-dd hh:mm")));
                    const qint64 sid =
                        Persistence::instance().createSession(
                            std::nullopt, fallbackTitle,
                            QDir::currentPath());
                    setActiveSession(sid);
                }
            }
        }
    } catch (const std::exception&) {
    }
}

bool MainWindow::confirmCwdRetarget(int dirtyCount, bool& doSave) {
    doSave = false;
    if (dirtyCount <= 0) return true;  // nothing to ask about

    ThemedQtDialog dlg(this);
    dlg.setWindowTitle(tr("Change Working Directory"));
    auto* lay = new QVBoxLayout(&dlg);
    auto* label = new QLabel(
        tr("This session has %n unsaved buffer(s). Retargeting "
           "the working directory will close these buffers.",
           "", dirtyCount),
        &dlg);
    label->setWordWrap(true);
    lay->addWidget(label);

    auto* btnRow = new QHBoxLayout();
    auto* saveBtn =
        new QPushButton(tr("Save all and retarget"), &dlg);
    auto* discardBtn =
        new QPushButton(tr("Discard and retarget"), &dlg);
    auto* cancelBtn = new QPushButton(tr("Cancel"), &dlg);
    dlg.setAccentButton(saveBtn);
    dlg.setDestructiveButton(discardBtn);
    btnRow->addStretch(1);
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(discardBtn);
    btnRow->addWidget(saveBtn);
    lay->addLayout(btnRow);

    // Use dialog custom result codes so we can tell which branch
    // the user picked without additional state.
    enum Choice { ChoiceCancel = 0, ChoiceDiscard = 1, ChoiceSave = 2 };
    connect(saveBtn, &QPushButton::clicked, &dlg,
            [&dlg]() { dlg.done(ChoiceSave); });
    connect(discardBtn, &QPushButton::clicked, &dlg,
            [&dlg]() { dlg.done(ChoiceDiscard); });
    connect(cancelBtn, &QPushButton::clicked, &dlg,
            [&dlg]() { dlg.done(ChoiceCancel); });

    const int rc = dlg.exec();
    if (rc == ChoiceCancel) return false;
    doSave = (rc == ChoiceSave);
    return true;
}

void MainWindow::onChangeSessionCwd() {
    const qint64 sid =
        m_currentSessionId > 0 ? m_currentSessionId : selectedSessionId();
    if (sid <= 0) return;

    QString startDir = QDir::homePath();
    const auto sessions = Persistence::instance().listSessions();
    for (const auto& s : sessions) {
        if (s.id == sid) {
            if (!s.cwd.isEmpty()) startDir = s.cwd;
            break;
        }
    }

    const QString newCwd = QFileDialog::getExistingDirectory(
        this, tr("Change Working Directory"), startDir);
    if (newCwd.isEmpty()) return;

    auto* pane = editorPaneWidget();
    const int dirty = pane ? pane->dirtyEditorCount() : 0;
    bool doSave = false;
    if (!confirmCwdRetarget(dirty, doSave)) return;

    if (pane && dirty > 0) {
        if (doSave) {
            if (!pane->saveAllDirty()) {
                // Abort the retarget rather than silently leaving
                // buffers on disk in a half-saved state.
                return;
            }
        }
        pane->discardAllAndCloseAll();
    }

    try {
        Persistence::instance().retargetSessionCwd(sid, newCwd);
    } catch (const std::exception&) {
        return;
    }

    if (m_statusCwdLabel && sid == m_currentSessionId) {
        m_statusCwdLabel->setText(newCwd);
    }
}

void MainWindow::onMoveSessionToProject() {
    const qint64 sid =
        m_currentSessionId > 0 ? m_currentSessionId : selectedSessionId();
    if (sid <= 0) return;

    ThemedQtDialog dlg(this);
    dlg.setWindowTitle(tr("Move Session to Project"));
    auto* lay = new QVBoxLayout(&dlg);
    lay->addWidget(new QLabel(tr("Target project:"), &dlg));
    auto* combo = new QComboBox(&dlg);
    combo->setObjectName(QStringLiteral("moveSessionCombo"));
    combo->addItem(tr("(orphan — no parent)"),
                   QVariant::fromValue<qlonglong>(0));
    for (const auto& p : Persistence::instance().listProjects()) {
        combo->addItem(p.name, QVariant::fromValue<qlonglong>(p.id));
    }
    lay->addWidget(combo);

    auto* box = dlg.buildButtonBox(QDialogButtonBox::Ok |
                                   QDialogButtonBox::Cancel);
    if (auto* ok = box->button(QDialogButtonBox::Ok)) {
        dlg.setAccentButton(ok);
    }
    lay->addWidget(box);

    if (dlg.exec() != QDialog::Accepted) return;
    const qlonglong chosen = combo->currentData().toLongLong();
    std::optional<qint64> target;
    if (chosen > 0) target = chosen;

    try {
        Persistence::instance().moveSessionToProject(sid, target);
    } catch (const std::exception&) {
    }
}

void MainWindow::onTreeContextMenu(const QPoint& pos) {
    if (!m_projectTree) return;
    const QModelIndex idx = m_projectTree->indexAt(pos);
    if (idx.isValid() &&
        m_projectTree->selectionModel()->currentIndex() != idx) {
        m_projectTree->setCurrentIndex(idx);
    }

    QMenu menu(this);
    const int kind = idx.isValid()
                         ? idx.data(ProjectSessionTreeModel::ItemTypeRole)
                               .toInt()
                         : -1;

    if (kind == ProjectSessionTreeModel::Project) {
        auto* renameAction = menu.addAction(tr("Rename"));
        connect(renameAction, &QAction::triggered, this,
                &MainWindow::onRenameSelected);
        auto* newSubAction = menu.addAction(tr("New Sub-Project…"));
        connect(newSubAction, &QAction::triggered, this,
                &MainWindow::onNewProject);
        auto* newSessAction = menu.addAction(tr("New Session Here"));
        connect(newSessAction, &QAction::triggered, this,
                &MainWindow::onNewSession);
        menu.addSeparator();
        auto* deleteAction = menu.addAction(tr("Delete…"));
        connect(deleteAction, &QAction::triggered, this,
                &MainWindow::onDeleteSelected);
    } else if (kind == ProjectSessionTreeModel::Session) {
        auto* renameAction = menu.addAction(tr("Rename"));
        connect(renameAction, &QAction::triggered, this,
                &MainWindow::onRenameSelected);
        auto* cwdAction =
            menu.addAction(tr("Change Working Directory…"));
        connect(cwdAction, &QAction::triggered, this,
                &MainWindow::onChangeSessionCwd);
        auto* moveAction = menu.addAction(tr("Move to Project…"));
        connect(moveAction, &QAction::triggered, this,
                &MainWindow::onMoveSessionToProject);
        menu.addSeparator();
        auto* deleteAction = menu.addAction(tr("Delete…"));
        connect(deleteAction, &QAction::triggered, this,
                &MainWindow::onDeleteSelected);
    } else {
        // Empty space — only the creation verbs make sense.
        auto* newProjAction = menu.addAction(tr("New Project…"));
        connect(newProjAction, &QAction::triggered, this,
                &MainWindow::onNewProject);
        auto* newSessAction = menu.addAction(tr("New Session"));
        connect(newSessAction, &QAction::triggered, this,
                &MainWindow::onNewSession);
    }

    menu.exec(m_projectTree->viewport()->mapToGlobal(pos));
}

// --- File URL drop → editor -------------------------------------

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    const QMimeData* md = event->mimeData();
    if (!md || !md->hasUrls()) {
        event->ignore();
        return;
    }
    for (const QUrl& u : md->urls()) {
        if (u.isLocalFile()) {
            event->acceptProposedAction();
            return;
        }
    }
    event->ignore();
}

void MainWindow::dropEvent(QDropEvent* event) {
    const QMimeData* md = event->mimeData();
    if (!md || !md->hasUrls()) {
        event->ignore();
        return;
    }
    bool opened = false;
    for (const QUrl& u : md->urls()) {
        if (!u.isLocalFile()) continue;
        const QString path = u.toLocalFile();
        if (path.isEmpty()) continue;
        if (openFile(path)) opened = true;
    }
    if (opened) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void MainWindow::flushPendingGeometryWrites() {
    // Belt-and-braces per §3.4: the debounced write path handles
    // the hot-exit case; this flush handles the clean-shutdown
    // case so the very last resize/drag is captured even if the
    // user closes the window before the debounce interval.
    if (m_windowGeometryTimer && m_windowGeometryTimer->isActive()) {
        m_windowGeometryTimer->stop();
        saveWindowGeometryNow();
    }
    if (m_windowPositionTimer && m_windowPositionTimer->isActive()) {
        m_windowPositionTimer->stop();
        saveWindowPositionNow();
    }
    if (m_leftPaneWidthTimer && m_leftPaneWidthTimer->isActive()) {
        m_leftPaneWidthTimer->stop();
        saveLeftPaneWidthNow();
    }
    if (m_topSplitterRatioTimer && m_topSplitterRatioTimer->isActive()) {
        m_topSplitterRatioTimer->stop();
        saveTopSplitterRatioNow();
    }
    if (m_chatSplitterWriteTimer && m_chatSplitterWriteTimer->isActive()) {
        m_chatSplitterWriteTimer->stop();
        saveChatSplitterStateNow();
    }
}
