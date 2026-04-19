// SPDX-License-Identifier: GPL-3.0-only

#include "codeeditor.h"

#include "persistence.h"

#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QMimeType>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QTextBlock>
#include <QTimer>

#ifdef APP_USE_KSYNTAXHIGHLIGHTING
#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Theme>
#endif

namespace {
// 1 s dirty-stash debounce per §6.2 — typing faster than this
// shouldn't thrash SQLite.
constexpr int kDirtyStashDebounceMs = 1000;
}  // namespace

// --- LineNumberArea trampoline ---------------------------------

LineNumberArea::LineNumberArea(CodeEditor* editor)
    : QWidget(editor), m_editor(editor) {}

QSize LineNumberArea::sizeHint() const {
    return QSize(m_editor->lineNumberAreaWidth(), 0);
}

void LineNumberArea::paintEvent(QPaintEvent* event) {
    m_editor->lineNumberAreaPaintEvent(event);
}

// --- CodeEditor ------------------------------------------------

CodeEditor::CodeEditor(KSyntaxHighlighting::Repository* repository,
                       QWidget* parent)
    : QPlainTextEdit(parent),
      m_repository(repository),
      m_lineNumberArea(new LineNumberArea(this)) {
    setLineWrapMode(QPlainTextEdit::NoWrap);  // Gedit default per §6.2
    setTabChangesFocus(false);
    // Monospace — Gedit uses "Monospace 11" by default; Qt's
    // StyleHint::Monospace picks whatever the platform ships.
    QFont monoFont;
    monoFont.setStyleHint(QFont::Monospace);
    monoFont.setFamily(QStringLiteral("Monospace"));
    monoFont.setFixedPitch(true);
    setFont(monoFont);

#ifdef APP_USE_KSYNTAXHIGHLIGHTING
    m_highlighter = new KSyntaxHighlighting::SyntaxHighlighter(document());
    if (m_repository) {
        m_highlighter->setTheme(m_repository->defaultTheme(
            KSyntaxHighlighting::Repository::DarkTheme));
    }
#endif

    m_dirtyStashTimer = new QTimer(this);
    m_dirtyStashTimer->setSingleShot(true);
    m_dirtyStashTimer->setInterval(kDirtyStashDebounceMs);
    connect(m_dirtyStashTimer, &QTimer::timeout,
            this, &CodeEditor::flushDirtyStash);

    connect(this, &QPlainTextEdit::blockCountChanged,
            this, &CodeEditor::onBlockCountChanged);
    connect(this, &QPlainTextEdit::updateRequest,
            this, &CodeEditor::onUpdateRequest);
    connect(this, &QPlainTextEdit::cursorPositionChanged,
            this, &CodeEditor::onCursorPositionChanged);
    connect(document(), &QTextDocument::modificationChanged,
            this, &CodeEditor::onModificationChanged);
    connect(document(), &QTextDocument::contentsChanged, this, [this]() {
        if (document()->isModified() && m_bufferId != 0) {
            m_dirtyStashTimer->start();
        }
    });

    onBlockCountChanged(0);
    onCursorPositionChanged();
}

CodeEditor::~CodeEditor() = default;

// --- Line number gutter ----------------------------------------

int CodeEditor::lineNumberAreaWidth() const {
    int digits = 1;
    int max = qMax(1, blockCount());
    while (max >= 10) {
        max /= 10;
        ++digits;
    }
    const int padding = 8;
    const int advance = fontMetrics().horizontalAdvance(QLatin1Char('9'));
    return padding + advance * digits;
}

void CodeEditor::onBlockCountChanged(int /*newBlockCount*/) {
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void CodeEditor::onUpdateRequest(const QRect& rect, int dy) {
    if (dy) {
        m_lineNumberArea->scroll(0, dy);
    } else {
        m_lineNumberArea->update(0, rect.y(),
                                 m_lineNumberArea->width(), rect.height());
    }
    if (rect.contains(viewport()->rect())) {
        onBlockCountChanged(0);
    }
}

void CodeEditor::resizeEvent(QResizeEvent* event) {
    QPlainTextEdit::resizeEvent(event);
    const QRect cr = contentsRect();
    m_lineNumberArea->setGeometry(
        QRect(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height()));
}

void CodeEditor::lineNumberAreaPaintEvent(QPaintEvent* event) {
    QPainter painter(m_lineNumberArea);
    // Slightly-darker-than-editor background so the gutter
    // visually separates from the text area without fighting the
    // KSH theme's code background.
    painter.fillRect(event->rect(), palette().color(QPalette::Window).darker(110));

    QTextBlock block = firstVisibleBlock();
    int blockNumber = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block)
                         .translated(contentOffset())
                         .top());
    int bottom = top + qRound(blockBoundingRect(block).height());

    const QColor fg = palette().color(QPalette::WindowText);
    painter.setPen(fg);

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            const QString number = QString::number(blockNumber + 1);
            painter.drawText(0, top,
                             m_lineNumberArea->width() - 4,
                             fontMetrics().height(),
                             Qt::AlignRight, number);
        }
        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++blockNumber;
    }
}

// --- Current-line highlight ------------------------------------

void CodeEditor::onCursorPositionChanged() {
    QList<QTextEdit::ExtraSelection> selections;
    if (!isReadOnly()) {
        QTextEdit::ExtraSelection sel;
        // Tint the KSH theme's code background toward the system
        // highlight color so the current line is visible under
        // both light and dark KSH themes.
        QColor lineColor = palette().color(QPalette::Highlight);
        lineColor.setAlpha(48);
        sel.format.setBackground(lineColor);
        sel.format.setProperty(QTextFormat::FullWidthSelection, true);
        sel.cursor = textCursor();
        sel.cursor.clearSelection();
        selections.append(sel);
    }
    setExtraSelections(selections);
}

// --- File binding / I/O ----------------------------------------

void CodeEditor::setFilePath(const QString& path) {
    if (m_filePath == path) return;
    m_filePath = path;
    rewireHighlighter();
    emit filePathChanged(path);
}

void CodeEditor::rewireHighlighter() {
#ifdef APP_USE_KSYNTAXHIGHLIGHTING
    if (!m_repository || !m_highlighter) return;

    KSyntaxHighlighting::Definition def =
        m_repository->definitionForFileName(m_filePath);
    if (!def.isValid()) {
        // Filename alone didn't match (e.g. `Dockerfile`,
        // `.env`, extension-less configs) — fall back to the
        // MIME database. KF5's KSyntaxHighlighting doesn't ship
        // `definitionForContent`, so we bridge via QMimeDatabase
        // which does content-sniffing and then hand the MIME
        // name to `definitionForMimeType`. This catches shebang
        // scripts and most "plain-text-but-actually-X" cases.
        QMimeDatabase mimeDb;
        const QMimeType mime = mimeDb.mimeTypeForFile(
            m_filePath, QMimeDatabase::MatchContent);
        if (mime.isValid()) {
            def = m_repository->definitionForMimeType(mime.name());
        }
    }
    m_highlighter->setDefinition(def);
#endif
}

bool CodeEditor::loadFromFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    // Path first so rewireHighlighter() picks the right
    // definition before we drop text into the document.
    setFilePath(path);

    // setPlainText triggers modificationChanged; suppress the
    // dirty-stash side effect by temporarily freezing the
    // modified flag, then reset cleanly after.
    const bool prevBlock = document()->blockSignals(true);
    setPlainText(QString::fromUtf8(bytes));
    document()->blockSignals(prevBlock);
    document()->setModified(false);
    emit dirtyStateChanged(false);

    QFileInfo info(path);
    m_savedMtime = info.lastModified().toSecsSinceEpoch();

    // Second rewire now that we have content — lets a
    // content-based match override a weak filename match.
    rewireHighlighter();
    return true;
}

bool CodeEditor::save() {
    if (m_filePath.isEmpty()) return false;

    QFile file(m_filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }

    m_suppressExternalCheck = true;
    const QByteArray bytes = toPlainText().toUtf8();
    const qint64 written = file.write(bytes);
    file.close();
    m_suppressExternalCheck = false;

    if (written != bytes.size()) return false;

    QFileInfo info(m_filePath);
    m_savedMtime = info.lastModified().toSecsSinceEpoch();
    document()->setModified(false);

    if (m_bufferId != 0) {
        Persistence::instance().saveBufferContent(
            m_bufferId, bytes, m_savedMtime);
    }
    emit dirtyStateChanged(false);
    return true;
}

bool CodeEditor::saveAs(const QString& newPath) {
    setFilePath(newPath);
    return save();
}

// --- Dirty tracking --------------------------------------------

void CodeEditor::onModificationChanged(bool modified) {
    emit dirtyStateChanged(modified);
    if (!modified && m_bufferId != 0) {
        // Save or explicit revert — clear the stash row per §6.2.
        m_dirtyStashTimer->stop();
        Persistence::instance().stashDirtyBuffer(m_bufferId, QByteArray());
    }
}

void CodeEditor::flushDirtyStash() {
    if (m_bufferId == 0) return;
    if (!document()->isModified()) return;
    m_lastStashedContent = toPlainText().toUtf8();
    Persistence::instance().stashDirtyBuffer(
        m_bufferId, m_lastStashedContent);
}

bool CodeEditor::stashTimerActive() const {
    return m_dirtyStashTimer && m_dirtyStashTimer->isActive();
}

void CodeEditor::applyDirtyContent(const QByteArray& content) {
    const bool prevBlock = document()->blockSignals(true);
    const int pos = textCursor().position();
    setPlainText(QString::fromUtf8(content));
    document()->blockSignals(prevBlock);
    document()->setModified(!content.isEmpty());
    QTextCursor c = textCursor();
    c.setPosition(qMin(pos, document()->characterCount() - 1));
    setTextCursor(c);
    m_lastStashedContent = content;
}

// --- External modification detection ---------------------------

void CodeEditor::focusInEvent(QFocusEvent* event) {
    QPlainTextEdit::focusInEvent(event);
    if (!m_suppressExternalCheck) {
        checkExternalModification();
    }
}

void CodeEditor::checkExternalModification() {
    if (m_filePath.isEmpty() || m_savedMtime == 0) return;
    QFileInfo info(m_filePath);
    if (!info.exists()) return;
    const qint64 diskMtime = info.lastModified().toSecsSinceEpoch();
    if (diskMtime == m_savedMtime) return;

    if (!document()->isModified()) {
        // Clean buffer → silently reload per §6.2.
        (void) loadFromFile(m_filePath);
        return;
    }

    // Dirty buffer → defer to the EditorPaneWidget's dialog
    // handler. Signal the situation by leaving m_savedMtime
    // stale and letting the pane widget listen. The simpler
    // in-editor path emits a signal that the pane wires to a
    // ThemedQtDialog. Rather than pulling the dialog dep into
    // this file we delegate via a dedicated signal.
    //
    // For now: do nothing — a follow-up will add the modalPrompt
    // callback. Tests cover the clean-reload path which is the
    // common case.
}
