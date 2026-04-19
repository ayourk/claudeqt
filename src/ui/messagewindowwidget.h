// SPDX-License-Identifier: GPL-3.0-only
//
// MessageWindowWidget — read-only view of a session's message
// history.
//
// Current scope:
//   - Yellow banner at top indicating the chat shell is not yet
//     wired to Claude
//   - QPlainTextEdit (read-only) below, rendering one message
//     per paragraph as `[ISO-timestamp] role: content`
//   - setCurrentSessionId(id) repopulates from listMessagesForSession
//   - Listens to Persistence::messagesAppended and appends in
//     place rather than re-querying the whole history
//   - Autoscroll only when the scrollbar is already near the
//     bottom
//
// A future release will replace the body with a QListView + custom
// delegates; the public API (setCurrentSessionId, clear, rerender)
// is the contract that survives.

#pragma once

#include <QString>
#include <QWidget>
#include <QtGlobal>

class QLabel;
class QPlainTextEdit;

class MessageWindowWidget : public QWidget {
    Q_OBJECT
public:
    explicit MessageWindowWidget(QWidget* parent = nullptr);
    ~MessageWindowWidget() override;

    // 0 means "no active session" — clears the view. Any other
    // id rerenders from Persistence::listMessagesForSession.
    void setCurrentSessionId(qint64 sessionId);
    qint64 currentSessionId() const { return m_sessionId; }

    // Accessors used by tests to inspect rendered text.
    QPlainTextEdit* messageView() const { return m_view; }
    QLabel* banner() const { return m_banner; }

public slots:
    // Re-read the whole history for the current session and
    // replace the view's contents. Also invoked explicitly after
    // setCurrentSessionId.
    void rerender();

private slots:
    void onMessagesAppended(qint64 sessionId, qint64 messageId);

private:
    void appendFormattedMessage(const QString& role,
                                const QByteArray& content,
                                qint64 createdSecs);
    bool isScrollNearBottom() const;
    void scrollToBottom();

    qint64 m_sessionId = 0;
    QLabel* m_banner = nullptr;
    QPlainTextEdit* m_view = nullptr;
};
