// SPDX-License-Identifier: GPL-3.0-only

#include "editorpanewidget.h"

#include "codeeditor.h"
#include "persistence.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QMessageBox>
#include <QSet>
#include <QFontMetricsF>
#include <QStackedWidget>
#include <QTabBar>
#include <QVBoxLayout>

#ifdef APP_USE_KSYNTAXHIGHLIGHTING
#include <KSyntaxHighlighting/Repository>
#endif

const QString& EditorPaneWidget::defaultFontFamily() {
    static const QString s = QStringLiteral("Monospace");
    return s;
}

EditorPaneWidget::EditorPaneWidget(QWidget* parent)
    : QWidget(parent)
#ifdef APP_USE_KSYNTAXHIGHLIGHTING
      , m_repository(new KSyntaxHighlighting::Repository())
#endif
{
    setObjectName(QStringLiteral("editorPane"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_tabBar = new QTabBar(this);
    m_tabBar->setObjectName(QStringLiteral("editorTabBar"));
    m_tabBar->setTabsClosable(true);
    m_tabBar->setMovable(true);
    m_tabBar->setExpanding(false);
    m_tabBar->setDocumentMode(true);
    layout->addWidget(m_tabBar);

    m_stack = new QStackedWidget(this);
    m_stack->setObjectName(QStringLiteral("editorStack"));
    layout->addWidget(m_stack, /*stretch=*/1);

    connect(m_tabBar, &QTabBar::currentChanged,
            this, &EditorPaneWidget::onTabBarCurrentChanged);
    connect(m_tabBar, &QTabBar::tabCloseRequested,
            this, &EditorPaneWidget::onTabCloseRequested);
    connect(m_tabBar, &QTabBar::tabMoved,
            this, &EditorPaneWidget::onTabMoved);

    connect(&Persistence::instance(), &Persistence::settingChanged,
            this, [this](const QString& name) {
                if (name.startsWith(QStringLiteral("editor."))) {
                    applyEditorSettings();
                }
            });

    connect(&Persistence::instance(), &Persistence::buffersChanged,
            this, &EditorPaneWidget::onBuffersChanged);

    applyEditorSettings();
}

EditorPaneWidget::~EditorPaneWidget() {
    delete m_repository;
    m_repository = nullptr;
}

int EditorPaneWidget::tabCount() const {
    return m_tabBar->count();
}

CodeEditor* EditorPaneWidget::currentEditor() const {
    const int idx = m_tabBar->currentIndex();
    if (idx < 0) return nullptr;
    return qobject_cast<CodeEditor*>(
        m_tabBar->tabData(idx).value<QWidget*>());
}

int EditorPaneWidget::indexOfEditor(CodeEditor* editor) const {
    for (int i = 0; i < m_stack->count(); ++i) {
        if (m_stack->widget(i) == editor) return i;
    }
    return -1;
}

int EditorPaneWidget::indexOfPath(const QString& path) const {
    const QString canonical = QFileInfo(path).absoluteFilePath();
    for (int i = 0; i < m_stack->count(); ++i) {
        auto* ed = qobject_cast<CodeEditor*>(m_stack->widget(i));
        if (!ed) continue;
        if (QFileInfo(ed->filePath()).absoluteFilePath() == canonical) {
            return i;
        }
    }
    return -1;
}

CodeEditor* EditorPaneWidget::openFile(const QString& path) {
    // Dedup — raise the existing tab if the file is already open.
    const int existing = indexOfPath(path);
    if (existing >= 0) {
        m_tabBar->setCurrentIndex(existing);
        return qobject_cast<CodeEditor*>(m_stack->widget(existing));
    }

    auto* editor = new CodeEditor(m_repository, this);
    editor->setLineWrapMode(m_wrapMode);
    if (!editor->loadFromFile(path)) {
        delete editor;
        return nullptr;
    }

    connect(editor, &CodeEditor::filePathChanged,
            this, &EditorPaneWidget::onEditorFilePathChanged);
    connect(editor, &CodeEditor::dirtyStateChanged,
            this, &EditorPaneWidget::onEditorDirtyChanged);

    m_stack->addWidget(editor);
    const QString basename = QFileInfo(path).fileName();
    const int tabIndex = m_tabBar->addTab(basename);
    m_tabBar->setTabToolTip(tabIndex, path);
    m_tabBar->setTabData(tabIndex,
                         QVariant::fromValue<QWidget*>(editor));
    m_tabBar->setCurrentIndex(tabIndex);
    m_stack->setCurrentWidget(editor);

    if (m_sessionId != 0) {
        m_suppressBufferSignal = true;
        qint64 bid = 0;
        for (const auto& b :
             Persistence::instance().listBuffersForSession(m_sessionId)) {
            if (b.filePath == path) { bid = b.id; break; }
        }
        if (bid == 0)
            bid = Persistence::instance().openBuffer(m_sessionId, path);
        editor->setBufferId(bid);
        editor->setTabPosition(tabIndex);
        Persistence::instance().setBufferTabPosition(bid, tabIndex);
        m_suppressBufferSignal = false;
    }

    applyEditorSettings();

    // Drop any pristine untitled tabs (empty doc, no path, not
    // modified) — those are placeholder tabs from toggleEditor()
    // and the user's first real open should replace, not stack on
    // top of, them. Iterate backwards so index math survives the
    // removals. Skip the tab we just opened.
    for (int i = m_stack->count() - 1; i >= 0; --i) {
        auto* ed = qobject_cast<CodeEditor*>(m_stack->widget(i));
        if (!ed || ed == editor) continue;
        if (ed->filePath().isEmpty() &&
            ed->document()->isEmpty() &&
            !ed->document()->isModified()) {
            closeTab(i);
        }
    }

    emit tabsChanged();
    return editor;
}

CodeEditor* EditorPaneWidget::newUntitled() {
    auto* editor = new CodeEditor(m_repository, this);
    editor->setLineWrapMode(m_wrapMode);

    connect(editor, &CodeEditor::filePathChanged,
            this, &EditorPaneWidget::onEditorFilePathChanged);
    connect(editor, &CodeEditor::dirtyStateChanged,
            this, &EditorPaneWidget::onEditorDirtyChanged);

    int untitledCount = 0;
    for (int i = 0; i < m_stack->count(); ++i) {
        auto* ed = qobject_cast<CodeEditor*>(m_stack->widget(i));
        if (ed && ed->filePath().isEmpty()) ++untitledCount;
    }
    const QString label = untitledCount == 0
        ? tr("Untitled")
        : tr("Untitled %1").arg(untitledCount + 1);

    m_stack->addWidget(editor);
    const int tabIndex = m_tabBar->addTab(label);
    m_tabBar->setTabData(tabIndex,
                         QVariant::fromValue<QWidget*>(editor));
    m_tabBar->setCurrentIndex(tabIndex);
    m_stack->setCurrentWidget(editor);

    if (m_sessionId != 0) {
        m_suppressBufferSignal = true;
        const qint64 bid = Persistence::instance().openBuffer(
            m_sessionId);
        editor->setBufferId(bid);
        editor->setTabPosition(tabIndex);
        Persistence::instance().setBufferTabPosition(bid, tabIndex);
        m_suppressBufferSignal = false;
    }

    applyEditorSettings();

    emit tabsChanged();
    return editor;
}

void EditorPaneWidget::closeTab(int index) {
    if (index < 0 || index >= m_tabBar->count()) return;
    auto* editor = qobject_cast<CodeEditor*>(
        m_tabBar->tabData(index).value<QWidget*>());

    m_suppressBufferSignal = true;

    if (editor && editor->bufferId() != 0 &&
        editor->document()->isModified()) {
        Persistence::instance().stashDirtyBuffer(
            editor->bufferId(), editor->toPlainText().toUtf8());
    }
    if (editor && editor->bufferId() != 0) {
        Persistence::instance().closeBuffer(editor->bufferId());
    }

    if (editor) {
        m_stack->removeWidget(editor);
        delete editor;
    }
    m_tabBar->removeTab(index);

    m_suppressBufferSignal = false;
    emit tabsChanged();
}

void EditorPaneWidget::setGlobalLineWrapMode(
    QPlainTextEdit::LineWrapMode mode) {
    m_wrapMode = mode;
    for (int i = 0; i < m_stack->count(); ++i) {
        if (auto* ed = qobject_cast<CodeEditor*>(m_stack->widget(i))) {
            ed->setLineWrapMode(mode);
        }
    }
}

void EditorPaneWidget::applyEditorSettings() {
    auto& db = Persistence::instance();
    auto asStr = [&db](const QString& key, const QString& fallback) {
        const QByteArray raw = db.getSetting(key);
        return raw.isEmpty() ? fallback : QString::fromUtf8(raw);
    };
    auto asInt = [&db](const QString& key, int fallback) {
        const QByteArray raw = db.getSetting(key);
        if (raw.isEmpty()) return fallback;
        bool ok = false;
        const int v = QString::fromUtf8(raw).toInt(&ok);
        return ok ? v : fallback;
    };

    QFont f;
    f.setStyleHint(QFont::Monospace);
    f.setFamily(asStr(QStringLiteral("editor.font_family"),
                      defaultFontFamily()));
    f.setFixedPitch(true);
    f.setPointSize(asInt(QStringLiteral("editor.font_size"),
                         kDefaultFontSize));

    const QString wrap =
        asStr(QStringLiteral("editor.line_wrap_mode"),
              QStringLiteral("nowrap"));
    m_wrapMode = (wrap == QStringLiteral("soft"))
                     ? QPlainTextEdit::WidgetWidth
                     : QPlainTextEdit::NoWrap;

    const int tabWidth =
        asInt(QStringLiteral("editor.tab_width"), kDefaultTabWidth);

    for (int i = 0; i < m_stack->count(); ++i) {
        auto* ed = qobject_cast<CodeEditor*>(m_stack->widget(i));
        if (!ed) continue;
        ed->setFont(f);
        ed->setLineWrapMode(m_wrapMode);
        const QFontMetricsF fm(f);
        ed->setTabStopDistance(fm.horizontalAdvance(QLatin1Char(' ')) *
                               tabWidth);
    }
}

void EditorPaneWidget::closePristineUntitledTabs() {
    for (int i = m_tabBar->count() - 1; i >= 0; --i) {
        auto* ed = qobject_cast<CodeEditor*>(
            m_tabBar->tabData(i).value<QWidget*>());
        if (!ed) continue;
        if (ed->filePath().isEmpty() &&
            ed->document()->isEmpty() &&
            !ed->document()->isModified()) {
            closeTab(i);
        }
    }
}

bool EditorPaneWidget::saveCurrent() {
    auto* ed = currentEditor();
    return ed ? ed->save() : false;
}

bool EditorPaneWidget::saveAsCurrent(const QString& newPath) {
    auto* ed = currentEditor();
    return ed ? ed->saveAs(newPath) : false;
}

int EditorPaneWidget::dirtyEditorCount() const {
    int n = 0;
    for (int i = 0; i < m_stack->count(); ++i) {
        if (auto* ed = qobject_cast<CodeEditor*>(m_stack->widget(i))) {
            if (ed->document()->isModified()) ++n;
        }
    }
    return n;
}

bool EditorPaneWidget::saveAllDirty() {
    bool ok = true;
    for (int i = 0; i < m_stack->count(); ++i) {
        auto* ed = qobject_cast<CodeEditor*>(m_stack->widget(i));
        if (!ed || !ed->document()->isModified()) continue;
        if (ed->filePath().isEmpty()) {
            // An unsaved new buffer — can't auto-save without
            // prompting for a path. §5.3 "Save all and retarget"
            // treats this as a failure so the caller can abort.
            ok = false;
            continue;
        }
        if (!ed->save()) ok = false;
    }
    return ok;
}

void EditorPaneWidget::discardAllAndCloseAll() {
    // Iterate front-to-back clearing modified flags first so the
    // closeTab path doesn't try to dirty-stash anything.
    for (int i = 0; i < m_stack->count(); ++i) {
        if (auto* ed = qobject_cast<CodeEditor*>(m_stack->widget(i))) {
            ed->document()->setModified(false);
        }
    }
    while (m_tabBar->count() > 0) {
        closeTab(0);
    }
}

// --- Slot plumbing ---------------------------------------------

void EditorPaneWidget::onTabBarCurrentChanged(int index) {
    if (index < 0 || index >= m_tabBar->count()) return;
    auto* w = m_tabBar->tabData(index).value<QWidget*>();
    if (w) m_stack->setCurrentWidget(w);
}

void EditorPaneWidget::onTabCloseRequested(int index) {
    if (index < 0 || index >= m_tabBar->count()) return;
    auto* editor = qobject_cast<CodeEditor*>(
        m_tabBar->tabData(index).value<QWidget*>());
    if (editor && editor->document()->isModified()) {
        const QString name = editor->filePath().isEmpty()
            ? tr("Untitled")
            : QFileInfo(editor->filePath()).fileName();
        const int choice = QMessageBox::question(
            this, tr("Unsaved Changes"),
            tr("Save changes to \"%1\"?").arg(name),
            QMessageBox::Save | QMessageBox::Discard |
                QMessageBox::Cancel,
            QMessageBox::Save);
        if (choice == QMessageBox::Cancel) return;
        if (choice == QMessageBox::Save) {
            if (editor->filePath().isEmpty()) {
                const QString path = QFileDialog::getSaveFileName(
                    this, tr("Save As"), QDir::currentPath());
                if (path.isEmpty()) return;
                if (!editor->saveAs(path)) return;
            } else {
                if (!editor->save()) return;
            }
        } else {
            const bool prev = editor->document()->blockSignals(true);
            editor->document()->setModified(false);
            editor->document()->blockSignals(prev);
        }
    }
    closeTab(index);
}

void EditorPaneWidget::onEditorFilePathChanged(const QString& newPath) {
    auto* ed = qobject_cast<CodeEditor*>(sender());
    const int idx = indexOfEditor(ed);
    if (idx < 0) return;
    m_tabBar->setTabToolTip(idx, newPath);
    refreshTabLabel(idx);
    emit tabsChanged();
}

void EditorPaneWidget::onEditorDirtyChanged(bool /*dirty*/) {
    auto* ed = qobject_cast<CodeEditor*>(sender());
    const int idx = indexOfEditor(ed);
    if (idx < 0) return;
    refreshTabLabel(idx);
    emit tabsChanged();
}

void EditorPaneWidget::onTabMoved(int /*from*/, int /*to*/) {
    for (int i = 0; i < m_tabBar->count(); ++i) {
        auto* ed = qobject_cast<CodeEditor*>(
            m_tabBar->tabData(i).value<QWidget*>());
        if (!ed || ed->bufferId() == 0) continue;
        if (ed->tabPosition() != i) {
            ed->setTabPosition(i);
            Persistence::instance().setBufferTabPosition(
                ed->bufferId(), i);
        }
    }
}

void EditorPaneWidget::refreshTabLabel(int index) {
    auto* ed = qobject_cast<CodeEditor*>(
        m_tabBar->tabData(index).value<QWidget*>());
    if (!ed) return;
    const QString base = ed->filePath().isEmpty()
        ? tr("Untitled")
        : QFileInfo(ed->filePath()).fileName();
    const QString label = ed->document()->isModified()
                              ? base + QLatin1Char('*')
                              : base;
    m_tabBar->setTabText(index, label);
}

// --- Session-scoped buffer persistence ----------------------------

void EditorPaneWidget::setCurrentSessionId(qint64 sessionId) {
    if (m_sessionId == sessionId) return;
    flushAndCloseAllTabs();
    m_sessionId = sessionId;
    if (m_sessionId != 0)
        restoreSessionBuffers();
}

void EditorPaneWidget::flushAndCloseAllTabs() {
    m_suppressBufferSignal = true;
    for (int i = 0; i < m_stack->count(); ++i) {
        auto* ed = qobject_cast<CodeEditor*>(m_stack->widget(i));
        if (!ed) continue;
        if (ed->bufferId() != 0 && ed->document()->isModified()) {
            Persistence::instance().stashDirtyBuffer(
                ed->bufferId(), ed->toPlainText().toUtf8());
        }
    }
    for (int i = 0; i < m_stack->count(); ++i) {
        if (auto* ed = qobject_cast<CodeEditor*>(m_stack->widget(i))) {
            const bool prev = ed->document()->blockSignals(true);
            ed->document()->setModified(false);
            ed->document()->blockSignals(prev);
        }
    }
    while (m_tabBar->count() > 0) {
        auto* w = m_stack->widget(0);
        m_stack->removeWidget(w);
        delete w;
        m_tabBar->removeTab(0);
    }
    m_suppressBufferSignal = false;
}

void EditorPaneWidget::restoreSessionBuffers() {
    const auto buffers =
        Persistence::instance().listBuffersForSession(m_sessionId);
    for (const auto& buf : buffers) {
        auto* editor = new CodeEditor(m_repository, this);
        editor->setLineWrapMode(m_wrapMode);

        if (buf.filePath.isEmpty()) {
            if (!buf.dirtyContent.isEmpty())
                editor->applyDirtyContent(buf.dirtyContent);
        } else {
            if (!editor->loadFromFile(buf.filePath)) {
                delete editor;
                continue;
            }
            if (!buf.dirtyContent.isEmpty())
                editor->applyDirtyContent(buf.dirtyContent);
        }

        editor->setBufferId(buf.id);
        editor->setTabPosition(buf.tabPosition);

        connect(editor, &CodeEditor::filePathChanged,
                this, &EditorPaneWidget::onEditorFilePathChanged);
        connect(editor, &CodeEditor::dirtyStateChanged,
                this, &EditorPaneWidget::onEditorDirtyChanged);

        const QString label = buf.filePath.isEmpty()
            ? tr("Untitled")
            : QFileInfo(buf.filePath).fileName();
        m_stack->addWidget(editor);
        const int tabIndex = m_tabBar->addTab(label);
        m_tabBar->setTabData(tabIndex,
                             QVariant::fromValue<QWidget*>(editor));
        refreshTabLabel(tabIndex);
    }
    if (m_tabBar->count() > 0) {
        m_tabBar->setCurrentIndex(0);
        auto* w = m_tabBar->tabData(0).value<QWidget*>();
        if (w) m_stack->setCurrentWidget(w);
    }
    applyEditorSettings();
    emit tabsChanged();
}

CodeEditor* EditorPaneWidget::editorForBufferId(qint64 bufferId) const {
    for (int i = 0; i < m_stack->count(); ++i) {
        auto* ed = qobject_cast<CodeEditor*>(m_stack->widget(i));
        if (ed && ed->bufferId() == bufferId) return ed;
    }
    return nullptr;
}

void EditorPaneWidget::onBuffersChanged(qint64 sessionId) {
    if (m_suppressBufferSignal) return;
    if (sessionId != m_sessionId) return;

    const auto rows =
        Persistence::instance().listBuffersForSession(m_sessionId);

    QSet<qint64> dbIds;
    for (const auto& row : rows) dbIds.insert(row.id);

    // Close tabs whose buffer was removed by another instance.
    for (int i = m_tabBar->count() - 1; i >= 0; --i) {
        auto* ed = qobject_cast<CodeEditor*>(
            m_tabBar->tabData(i).value<QWidget*>());
        if (!ed || ed->bufferId() == 0) continue;
        if (!dbIds.contains(ed->bufferId())) {
            ed->document()->setModified(false);
            m_stack->removeWidget(ed);
            delete ed;
            m_tabBar->removeTab(i);
        }
    }

    for (const auto& row : rows) {
        auto* ed = editorForBufferId(row.id);
        if (!ed) {
            // New buffer from another instance — open it.
            auto* editor = new CodeEditor(m_repository, this);
            editor->setLineWrapMode(m_wrapMode);
            if (row.filePath.isEmpty()) {
                if (!row.dirtyContent.isEmpty())
                    editor->applyDirtyContent(row.dirtyContent);
            } else {
                if (!editor->loadFromFile(row.filePath)) {
                    delete editor;
                    continue;
                }
                if (!row.dirtyContent.isEmpty())
                    editor->applyDirtyContent(row.dirtyContent);
            }
            editor->setBufferId(row.id);

            connect(editor, &CodeEditor::filePathChanged,
                    this, &EditorPaneWidget::onEditorFilePathChanged);
            connect(editor, &CodeEditor::dirtyStateChanged,
                    this, &EditorPaneWidget::onEditorDirtyChanged);

            editor->setTabPosition(row.tabPosition);
            m_stack->addWidget(editor);
            const QString tabLabel = row.filePath.isEmpty()
                ? tr("Untitled")
                : QFileInfo(row.filePath).fileName();
            const int ti = m_tabBar->addTab(tabLabel);
            m_tabBar->setTabData(ti,
                                 QVariant::fromValue<QWidget*>(editor));
            refreshTabLabel(ti);
            applyEditorSettings();
            continue;
        }

        // Existing tab — sync dirty content if changed externally.
        if (ed->stashTimerActive()) continue;
        if (row.dirtyContent == ed->lastStashedContent()) continue;
        ed->applyDirtyContent(row.dirtyContent);
        const int idx = indexOfEditor(ed);
        if (idx >= 0) refreshTabLabel(idx);
    }
    emit tabsChanged();
}
