// SPDX-License-Identifier: GPL-3.0-only

#include "messagewindowwidget.h"

#include "persistence.h"

#include <QDateTime>
#include <QLabel>
#include <QList>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QString>
#include <QVBoxLayout>

MessageWindowWidget::MessageWindowWidget(QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("messageWindow"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_banner = new QLabel(
        tr("Chat shell is not yet wired to Claude — your messages "
           "will be saved locally but not sent. Integration is "
           "planned for a future release."),
        this);
    m_banner->setObjectName(QStringLiteral("chatInertBanner"));
    m_banner->setWordWrap(true);
    m_banner->setAutoFillBackground(true);
    m_banner->setContentsMargins(8, 4, 8, 4);
    // Yellow background per §7.2. Palette is used rather than
    // a stylesheet so the ThemedQtDialog theme doesn't override
    // it — the banner is supposed to stand out.
    QPalette bp = m_banner->palette();
    bp.setColor(QPalette::Window, QColor(0xFF, 0xF2, 0xA8));
    bp.setColor(QPalette::WindowText, QColor(0x33, 0x2B, 0x00));
    m_banner->setPalette(bp);
    layout->addWidget(m_banner);

    m_view = new QPlainTextEdit(this);
    m_view->setObjectName(QStringLiteral("messageView"));
    m_view->setReadOnly(true);
    m_view->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    layout->addWidget(m_view, /*stretch=*/1);

    connect(&Persistence::instance(), &Persistence::messagesAppended,
            this, &MessageWindowWidget::onMessagesAppended);
}

MessageWindowWidget::~MessageWindowWidget() = default;

void MessageWindowWidget::setCurrentSessionId(qint64 sessionId) {
    if (m_sessionId == sessionId) return;
    m_sessionId = sessionId;
    rerender();
}

void MessageWindowWidget::rerender() {
    m_view->clear();
    if (m_sessionId == 0) return;

    const QList<MessageRow> rows =
        Persistence::instance().listMessagesForSession(m_sessionId);
    for (const auto& row : rows) {
        appendFormattedMessage(row.msgRole, row.content, row.created);
    }
    scrollToBottom();
}

void MessageWindowWidget::onMessagesAppended(qint64 sessionId,
                                             qint64 messageId) {
    if (sessionId != m_sessionId || m_sessionId == 0) return;

    // Re-read the single new row. listMessagesForSession is
    // cheap enough that we could also just rerender, but this
    // keeps the autoscroll dead-zone rule from firing on
    // unrelated history reloads.
    const bool wasNearBottom = isScrollNearBottom();

    QList<MessageRow> rows =
        Persistence::instance().listMessagesForSession(sessionId);
    for (const auto& row : rows) {
        if (row.id == messageId) {
            appendFormattedMessage(row.msgRole, row.content, row.created);
            break;
        }
    }

    if (wasNearBottom) scrollToBottom();
}

void MessageWindowWidget::appendFormattedMessage(const QString& role,
                                                 const QByteArray& content,
                                                 qint64 createdSecs) {
    const QDateTime dt =
        QDateTime::fromSecsSinceEpoch(createdSecs).toLocalTime();
    const QString ts = dt.toString(Qt::ISODate);
    const QString body = QString::fromUtf8(content);
    const QString block =
        QStringLiteral("[%1] %2: %3\n\n").arg(ts, role, body);
    m_view->appendPlainText(block.trimmed());
    // Preserve the double-newline separator the spec describes
    // by emitting a blank line between messages. appendPlainText
    // already adds one trailing newline per call; the .trimmed()
    // above removes the extra we put in `block`.
    QTextCursor cursor = m_view->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QStringLiteral("\n"));
}

bool MessageWindowWidget::isScrollNearBottom() const {
    QScrollBar* sb = m_view->verticalScrollBar();
    if (!sb) return true;
    // "Near the bottom" = within one viewport height of max per
    // §7.2. Tiny wheel nudges stay in the dead zone; a deliberate
    // scroll-up disables autoscroll.
    const int viewportH = m_view->viewport()->height();
    return (sb->maximum() - sb->value()) <= viewportH;
}

void MessageWindowWidget::scrollToBottom() {
    QScrollBar* sb = m_view->verticalScrollBar();
    if (sb) sb->setValue(sb->maximum());
}
