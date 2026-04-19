// SPDX-License-Identifier: GPL-3.0-only

#include "chatinputwidget.h"

#include "persistence.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QList>
#include <QMimeData>
#include <QStandardPaths>
#include <QString>
#include <QTextCursor>
#include <QTimer>
#include <QUrl>
#include <QUuid>

namespace {

QString attachmentsDirForSession(qint64 sessionId) {
    const QString root = QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation);
    return QDir(root).filePath(
        QStringLiteral("attachments/%1").arg(sessionId));
}

}  // namespace

ChatInputWidget::ChatInputWidget(QWidget* parent)
    : QPlainTextEdit(parent) {
    setObjectName(QStringLiteral("chatInput"));
    setPlaceholderText(
        tr("Type a message — Enter to send, Shift+Enter for a newline"));
    setAcceptDrops(true);
    setTabChangesFocus(true);

    m_draftDebounceTimer = new QTimer(this);
    m_draftDebounceTimer->setSingleShot(true);
    m_draftDebounceTimer->setInterval(kDraftDebounceMs);
    connect(m_draftDebounceTimer, &QTimer::timeout,
            this, &ChatInputWidget::flushDraftNow);

    connect(this, &QPlainTextEdit::textChanged,
            this, &ChatInputWidget::onTextChanged);

    connect(&Persistence::instance(), &Persistence::chatDraftChanged,
            this, &ChatInputWidget::onDraftChanged);
}

ChatInputWidget::~ChatInputWidget() = default;

void ChatInputWidget::setCurrentSessionId(qint64 sessionId) {
    if (m_sessionId == sessionId) return;

    // Flush any pending draft for the outgoing session before
    // we rebind so a rapid session switch doesn't leak a write
    // from session A into session B after the debounce fires.
    if (m_draftDebounceTimer->isActive()) {
        m_draftDebounceTimer->stop();
        flushDraftNow();
    }

    m_sessionId = sessionId;

    // Load the persisted draft for the new session.
    m_suppressDraftWrites = true;
    if (m_sessionId != 0) {
        const QByteArray draft =
            Persistence::instance().sessionChatDraft(m_sessionId);
        setPlainText(QString::fromUtf8(draft));
    } else {
        clear();
    }
    m_suppressDraftWrites = false;
}

void ChatInputWidget::submit() {
    if (m_sessionId == 0) {
        emit chatStatusMessage(
            tr("No active session — message not saved."));
        return;
    }
    const QString text = toPlainText().trimmed();
    if (text.isEmpty()) return;

    // Atomic append + clear draft per §7.3. The DAO promotes
    // any draft-scoped attachments referenced by the outbound
    // content to message scope inside the same transaction.
    Persistence::instance().appendMessageAndClearDraft(
        m_sessionId, QStringLiteral("user"), text.toUtf8());

    // Reset the input without tripping the draft debounce.
    m_suppressDraftWrites = true;
    clear();
    m_suppressDraftWrites = false;

    emit userMessageSubmitted(m_sessionId, text);
}

// --- Key handling ----------------------------------------------

void ChatInputWidget::keyPressEvent(QKeyEvent* event) {
    const bool isReturn = event->key() == Qt::Key_Return ||
                          event->key() == Qt::Key_Enter;
    if (isReturn) {
        // Shift+Enter → newline. Every other Enter variant
        // submits: plain Enter, Ctrl+Enter (Claude Code / Console
        // muscle memory per §7.2), and numpad Enter (which
        // arrives with Qt::KeypadModifier set — the previous
        // `modifiers() == NoModifier` check silently dropped it).
        if (event->modifiers() & Qt::ShiftModifier) {
            QPlainTextEdit::keyPressEvent(event);
            return;
        }
        submit();
        event->accept();
        return;
    }
    QPlainTextEdit::keyPressEvent(event);
}

// --- Paste + drop classification -------------------------------

void ChatInputWidget::insertFromMimeData(const QMimeData* source) {
    if (!source) return;
    if (classifyMime(source, /*fromDrop=*/false)) return;
    QPlainTextEdit::insertFromMimeData(source);
}

void ChatInputWidget::dragEnterEvent(QDragEnterEvent* event) {
    const QMimeData* md = event->mimeData();
    if (!md) {
        event->ignore();
        return;
    }
    // Accept the drag if we know we'll handle it. File URL drops
    // are routed up to the parent, so we must NOT accept here —
    // otherwise the dropEvent hits us instead of MainWindow.
    if (md->hasUrls()) {
        bool hasFile = false;
        bool hasWeb = false;
        for (const QUrl& u : md->urls()) {
            if (u.isLocalFile())
                hasFile = true;
            else
                hasWeb = true;
        }
        if (hasFile && !hasWeb) {
            event->ignore();  // propagate to MainWindow
            return;
        }
        event->acceptProposedAction();
        return;
    }
    if (md->hasImage()) {
        // Accept so we can reject cleanly in dropEvent with a
        // status message rather than silently rejecting the drag.
        event->acceptProposedAction();
        return;
    }
    if (md->hasText()) {
        event->acceptProposedAction();
        return;
    }
    event->ignore();
}

void ChatInputWidget::dropEvent(QDropEvent* event) {
    const QMimeData* md = event->mimeData();
    if (!md) {
        event->ignore();
        return;
    }
    if (classifyMime(md, /*fromDrop=*/true)) {
        event->acceptProposedAction();
        return;
    }
    // Unhandled — propagate up.
    event->ignore();
}

bool ChatInputWidget::classifyMime(const QMimeData* source, bool fromDrop) {
    // Images: rejected on both paste and drop per §7.3.
    if (source->hasImage()) {
        emit chatStatusMessage(
            tr("Image attachments are not yet supported."));
        return true;
    }

    // URLs: classify file:// vs http(s)://.
    if (source->hasUrls()) {
        QList<QUrl> webUrls;
        bool sawFile = false;
        for (const QUrl& u : source->urls()) {
            if (u.isLocalFile()) {
                sawFile = true;
            } else {
                webUrls.append(u);
            }
        }
        if (sawFile) {
            if (fromDrop) {
                // Drop: bail out so MainWindow's dropEvent can
                // open the file in the editor per §6.3 item 3.
                return false;
            }
            // Paste: rejected with status per §7.3.
            emit chatStatusMessage(
                tr("File attachments are not yet supported."));
            return true;
        }
        // Web URLs only → insert as plain text, one per line.
        if (!webUrls.isEmpty()) {
            QTextCursor cursor = textCursor();
            for (int i = 0; i < webUrls.size(); ++i) {
                if (i > 0) cursor.insertText(QStringLiteral("\n"));
                cursor.insertText(webUrls.at(i).toString());
            }
            return true;
        }
    }

    // Plain text: inline or spill to disk depending on size.
    if (source->hasText()) {
        const QString text = source->text();
        const qint64 utf8Len = text.toUtf8().size();
        if (utf8Len > kPasteSpillThresholdBytes) {
            if (!spillLargePaste(text)) {
                emit chatStatusMessage(
                    tr("Large paste could not be written to attachment "
                       "directory; inserting truncated text instead."));
                QTextCursor cursor = textCursor();
                cursor.insertText(text.left(
                    static_cast<int>(kPasteSpillThresholdBytes)));
            }
            return true;
        }
        QTextCursor cursor = textCursor();
        cursor.insertText(text);
        return true;
    }

    return false;
}

bool ChatInputWidget::spillLargePaste(const QString& text) {
    if (m_sessionId == 0) return false;

    const QString dir = attachmentsDirForSession(m_sessionId);
    if (!QDir().mkpath(dir)) return false;

    const QString uuid =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString relative = QStringLiteral("attachments/%1.txt").arg(uuid);
    const QString absolute = QDir(dir).filePath(uuid + QStringLiteral(".txt"));

    {
        QFile f(absolute);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return false;
        }
        const QByteArray bytes = text.toUtf8();
        if (f.write(bytes) != bytes.size()) {
            f.close();
            QFile::remove(absolute);
            return false;
        }
        f.close();
    }

    const qint64 sizeBytes = QFileInfo(absolute).size();
    Persistence::instance().insertDraftAttachment(
        m_sessionId, uuid, absolute,
        QStringLiteral("text/plain"), sizeBytes);

    const QString token = QStringLiteral(
        "[attached: %1 — %2 B, text/plain]")
                              .arg(relative)
                              .arg(sizeBytes);

    QTextCursor cursor = textCursor();
    cursor.insertText(token);
    return true;
}

// --- Draft debounce --------------------------------------------

void ChatInputWidget::onTextChanged() {
    if (m_suppressDraftWrites) return;
    if (m_sessionId == 0) return;
    m_draftDebounceTimer->start();
}

void ChatInputWidget::flushDraftNow() {
    if (m_sessionId == 0) return;
    m_lastFlushedDraft = toPlainText().toUtf8();
    Persistence::instance().setSessionChatDraft(
        m_sessionId, m_lastFlushedDraft);
}

void ChatInputWidget::onDraftChanged(qint64 sessionId) {
    if (sessionId != m_sessionId) return;
    if (m_draftDebounceTimer->isActive()) return;
    const QByteArray persisted =
        Persistence::instance().sessionChatDraft(m_sessionId);
    if (persisted == m_lastFlushedDraft) return;
    m_lastFlushedDraft = persisted;
    m_suppressDraftWrites = true;
    const int pos = textCursor().position();
    setPlainText(QString::fromUtf8(persisted));
    QTextCursor c = textCursor();
    c.setPosition(qMin(pos, document()->characterCount() - 1));
    setTextCursor(c);
    m_suppressDraftWrites = false;
}
