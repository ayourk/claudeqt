// SPDX-License-Identifier: GPL-3.0-only
//
// ChatInputWidget — auto-resizing multi-line chat input.
//
// Scope:
//   - QPlainTextEdit subclass with Enter=submit, Shift+Enter=newline,
//     Ctrl+Enter=submit (muscle-memory alias)
//   - setCurrentSessionId(id) → loads persisted chat_draft into
//     the buffer and binds subsequent write-through persistence
//   - 500 ms debounced setSessionChatDraft on textChanged
//   - insertFromMimeData: text > 8 KB spills to a temp file under
//     <AppLocalDataLocation>/attachments/<sessionId>/<uuid>.txt
//     and inserts a reference token; http/https URL pastes insert
//     as plain text; images reject with a status signal; file
//     URL pastes reject with a status signal
//   - dragEnter/drop classification mirrors paste with ONE
//     exception: file URL drops call event->ignore() so the
//     parent (MainWindow) receives the drop and opens the file
//     in the editor per §6.3 item 3
//   - Submit path: calls
//     Persistence::appendMessageAndClearDraft(sessionId, "user", …)
//     emits userMessageSubmitted(sessionId, text), then resets
//     the input to empty
//
// Deferred:
//   - Per-session switching: setCurrentSessionId is called once
//     at startup with the scratch session; active-session change
//     events from the tree will call it on switch
//   - Attachment cleanup when the draft is edited to remove a
//     spill token (tracked in persistence.h comments)

#pragma once

#include <QPlainTextEdit>
#include <QString>
#include <QtGlobal>

class QDragEnterEvent;
class QDropEvent;
class QKeyEvent;
class QMimeData;
class QTimer;

class ChatInputWidget : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit ChatInputWidget(QWidget* parent = nullptr);
    ~ChatInputWidget() override;

    // 0 = no active session (draft persistence inert, submit is
    // a no-op signal emission). Initially bound to the scratch
    // session MainWindow creates at startup.
    void setCurrentSessionId(qint64 sessionId);
    qint64 currentSessionId() const { return m_sessionId; }

    // Submit the current buffer. Called from key bindings and
    // from the "Send" button. No-op when the buffer is empty
    // (after trim) or when no session is bound.
    void submit();

    // Cap for inline paste. Pastes whose UTF-8 length exceeds
    // this are spilled to disk per §7.3 (default 8 KiB).
    static constexpr qint64 kPasteSpillThresholdBytes = 8 * 1024;

    // Debounce interval for chat_draft write-through persistence.
    static constexpr int kDraftDebounceMs = 500;

signals:
    // Emitted after a successful submit. The (future) Claude bridge
    // will connect to this to fire off the outbound request;
    // currently we only persist the message locally.
    void userMessageSubmitted(qint64 sessionId, const QString& text);

    // Emitted when paste/drop classification rejects content
    // with a user-visible reason. MainWindow's status bar
    // listens and flashes the message.
    void chatStatusMessage(const QString& text);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void insertFromMimeData(const QMimeData* source) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private slots:
    void onTextChanged();
    void flushDraftNow();
    void onDraftChanged(qint64 sessionId);

private:
    // Route MIME paste/drop content through the classifier.
    // Shared by insertFromMimeData (paste) and dropEvent (drop).
    // The `fromDrop` flag flips file-URL handling: paste rejects
    // them, drop ignores the event so the parent can handle it.
    // Returns true if the classifier fully handled the mime data
    // (inserted, spilled, or rejected); false means the caller
    // should propagate the event upward.
    bool classifyMime(const QMimeData* source, bool fromDrop);

    // Write `text.toUtf8()` to a new attachment file under the
    // session's attachment directory, register it as a draft
    // attachment, and insert a reference token in the buffer.
    // Returns true on success.
    bool spillLargePaste(const QString& text);

    qint64 m_sessionId = 0;
    QTimer* m_draftDebounceTimer = nullptr;
    QByteArray m_lastFlushedDraft;

    // Guards onTextChanged against re-entering the draft write
    // path while we're programmatically populating the buffer
    // (e.g., loadDraftForSession or spillLargePaste inserting
    // the reference token).
    bool m_suppressDraftWrites = false;
};
