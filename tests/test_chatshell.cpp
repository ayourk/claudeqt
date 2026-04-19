// SPDX-License-Identifier: GPL-3.0-only
//
// Unit tests for the chat shell. Covers:
//   - MainWindow bootstraps a scratch session and binds both
//     chat widgets to it
//   - ChatInputWidget draft persistence (debounced write-through
//     and synchronous load on setCurrentSessionId)
//   - Submit path: appendMessageAndClearDraft transactionally
//     posts the message, clears the draft, and emits
//     userMessageSubmitted
//   - Enter submits, Shift+Enter inserts newline, Ctrl+Enter
//     submits
//   - Paste: text below threshold inlines, text above threshold
//     spills to an attachment file and inserts a reference token
//   - Paste: http(s) URLs inline as plain text, file URLs reject
//     with a chatStatusMessage, images reject with a
//     chatStatusMessage
//   - MessageWindowWidget renders appended messages via the
//     Persistence::messagesAppended signal and respects the
//     autoscroll dead-zone rule

#include "chatinputwidget.h"
#include "instancehub.h"
#include "mainwindow.h"
#include "messagewindowwidget.h"
#include "persistence.h"

// Test shim exposing the protected insertFromMimeData path so we
// can drive the paste classifier without the clipboard round-trip.
class PublicChatInput : public ChatInputWidget {
public:
    using ChatInputWidget::ChatInputWidget;
    void publicInsertFromMimeData(const QMimeData* md) {
        insertFromMimeData(md);
    }
};

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QSignalSpy>
#include <QSplitter>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <QtTest/QtTest>

class TestChatShell : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void mainWindowBootstrapsScratchSession();
    void draftPersistsAcrossRebind();
    void submitAppendsMessageAndClearsDraft();
    void enterKeySubmits();
    void shiftEnterInsertsNewline();
    void ctrlEnterSubmits();
    void numpadEnterSubmits();
    void pasteInlineBelowThreshold();
    void pasteSpillAboveThreshold();
    void pasteWebUrlInlinesText();
    void pasteFileUrlRejects();
    void pasteImageRejects();
    void messageWindowRendersAppendedMessages();

private:
    QTemporaryDir m_tmp;
};

void TestChatShell::initTestCase() {
    QVERIFY(m_tmp.isValid());
    const QString dbPath = m_tmp.filePath(QStringLiteral("chat.sqlite"));
    Persistence::setDatabasePathForTesting(dbPath);
    QCoreApplication::setApplicationName(QStringLiteral("chatshell_test"));
    // Redirect AppLocalDataLocation so paste spill files land in
    // the temp dir rather than the real user profile.
    qputenv("XDG_DATA_HOME", m_tmp.path().toUtf8());

    const QString sockPath = m_tmp.filePath(QStringLiteral("hub.sock"));
    InstanceHub::setSocketPathForTesting(sockPath);
    InstanceHub::instance().start();

    (void) Persistence::instance();
    Persistence::instance().connectToHub();
    QVERIFY(QFile::exists(dbPath));
}

void TestChatShell::cleanupTestCase() {
    InstanceHub::instance().shutdown();
    InstanceHub::setSocketPathForTesting(QString());
}

// --- MainWindow bootstrap ----------------------------------------

void TestChatShell::mainWindowBootstrapsScratchSession() {
    MainWindow w;
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));

    QVERIFY(w.currentSessionId() > 0);
    QVERIFY(w.messageWindow() != nullptr);
    QVERIFY(w.chatInputWidget() != nullptr);
    QCOMPARE(w.messageWindow()->currentSessionId(), w.currentSessionId());
    QCOMPARE(w.chatInputWidget()->currentSessionId(), w.currentSessionId());
    QVERIFY(w.messageChatSplitter() != nullptr);
    QCOMPARE(w.messageChatSplitter()->orientation(), Qt::Vertical);
    QCOMPARE(w.messageChatSplitter()->count(), 2);
}

// --- Draft persistence -------------------------------------------

void TestChatShell::draftPersistsAcrossRebind() {
    const qint64 sid = Persistence::instance().createSession(
        std::nullopt, QStringLiteral("draft-test"), QDir::currentPath());

    PublicChatInput w;
    w.setCurrentSessionId(sid);
    w.setPlainText(QStringLiteral("half-composed prompt"));

    // Force the debounced flush synchronously rather than waiting
    // on the 500 ms timer.
    QMetaObject::invokeMethod(&w, "flushDraftNow", Qt::DirectConnection);

    const QByteArray stored =
        Persistence::instance().sessionChatDraft(sid);
    QCOMPARE(stored, QByteArray("half-composed prompt"));

    // Rebinding to the same session through a fresh widget should
    // reload the persisted draft into the buffer.
    ChatInputWidget w2;
    w2.setCurrentSessionId(sid);
    QCOMPARE(w2.toPlainText(), QStringLiteral("half-composed prompt"));
}

// --- Submit path --------------------------------------------------

void TestChatShell::submitAppendsMessageAndClearsDraft() {
    const qint64 sid = Persistence::instance().createSession(
        std::nullopt, QStringLiteral("submit-test"), QDir::currentPath());

    PublicChatInput w;
    w.setCurrentSessionId(sid);
    w.setPlainText(QStringLiteral("hello world"));
    QMetaObject::invokeMethod(&w, "flushDraftNow", Qt::DirectConnection);

    QSignalSpy spy(&w, &ChatInputWidget::userMessageSubmitted);
    w.submit();

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toLongLong(), sid);
    QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("hello world"));

    // Draft is cleared, message is appended.
    QCOMPARE(Persistence::instance().sessionChatDraft(sid), QByteArray());
    const auto rows =
        Persistence::instance().listMessagesForSession(sid);
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.first().msgRole, QStringLiteral("user"));
    QCOMPARE(rows.first().content, QByteArray("hello world"));
    QVERIFY(w.toPlainText().isEmpty());
}

// --- Key handling -------------------------------------------------

void TestChatShell::enterKeySubmits() {
    const qint64 sid = Persistence::instance().createSession(
        std::nullopt, QStringLiteral("enter-test"), QDir::currentPath());

    PublicChatInput w;
    w.setCurrentSessionId(sid);
    w.setPlainText(QStringLiteral("ping"));

    QSignalSpy spy(&w, &ChatInputWidget::userMessageSubmitted);
    QTest::keyClick(&w, Qt::Key_Return);
    QCOMPARE(spy.count(), 1);
    QVERIFY(w.toPlainText().isEmpty());
}

void TestChatShell::shiftEnterInsertsNewline() {
    const qint64 sid = Persistence::instance().createSession(
        std::nullopt, QStringLiteral("shift-test"), QDir::currentPath());

    PublicChatInput w;
    w.setCurrentSessionId(sid);
    w.setPlainText(QStringLiteral("a"));
    QTextCursor c = w.textCursor();
    c.movePosition(QTextCursor::End);
    w.setTextCursor(c);

    QSignalSpy spy(&w, &ChatInputWidget::userMessageSubmitted);
    QTest::keyClick(&w, Qt::Key_Return, Qt::ShiftModifier);
    QCOMPARE(spy.count(), 0);
    QVERIFY(w.toPlainText().contains(QLatin1Char('\n')));
}

void TestChatShell::ctrlEnterSubmits() {
    const qint64 sid = Persistence::instance().createSession(
        std::nullopt, QStringLiteral("ctrl-test"), QDir::currentPath());

    PublicChatInput w;
    w.setCurrentSessionId(sid);
    w.setPlainText(QStringLiteral("pong"));

    QSignalSpy spy(&w, &ChatInputWidget::userMessageSubmitted);
    QTest::keyClick(&w, Qt::Key_Return, Qt::ControlModifier);
    QCOMPARE(spy.count(), 1);
}

void TestChatShell::numpadEnterSubmits() {
    // Qt::Key_Enter with Qt::KeypadModifier is what the numpad
    // Enter key delivers. The original modifier check only
    // accepted NoModifier/ControlModifier, so numpad Enter fell
    // through to QPlainTextEdit and inserted a newline.
    const qint64 sid = Persistence::instance().createSession(
        std::nullopt, QStringLiteral("numpad-test"), QDir::currentPath());

    PublicChatInput w;
    w.setCurrentSessionId(sid);
    w.setPlainText(QStringLiteral("numpad"));

    QSignalSpy spy(&w, &ChatInputWidget::userMessageSubmitted);
    QTest::keyClick(&w, Qt::Key_Enter, Qt::KeypadModifier);
    QCOMPARE(spy.count(), 1);
    QVERIFY(w.toPlainText().isEmpty());
}

// --- Paste classification ----------------------------------------

void TestChatShell::pasteInlineBelowThreshold() {
    const qint64 sid = Persistence::instance().createSession(
        std::nullopt, QStringLiteral("paste-small"), QDir::currentPath());

    PublicChatInput w;
    w.setCurrentSessionId(sid);

    QMimeData md;
    md.setText(QStringLiteral("short paste"));
    w.publicInsertFromMimeData(&md);
    QVERIFY(w.toPlainText().contains(QStringLiteral("short paste")));
}

void TestChatShell::pasteSpillAboveThreshold() {
    const qint64 sid = Persistence::instance().createSession(
        std::nullopt, QStringLiteral("paste-big"), QDir::currentPath());

    PublicChatInput w;
    w.setCurrentSessionId(sid);

    QMimeData md;
    // 16 KiB of 'x', well above the 8 KiB threshold.
    md.setText(QString(16 * 1024, QLatin1Char('x')));
    w.publicInsertFromMimeData(&md);

    // Reference token inserted into the buffer.
    QVERIFY(w.toPlainText().contains(QStringLiteral("[attached:")));
    QVERIFY(w.toPlainText().contains(QStringLiteral("text/plain")));

    // Attachment row registered as draft-scoped.
    const auto attachments =
        Persistence::instance().listDraftAttachments(sid);
    QCOMPARE(attachments.size(), 1);
    QCOMPARE(attachments.first().mimeType, QStringLiteral("text/plain"));
    QVERIFY(attachments.first().sizeBytes >= 16 * 1024);
    QVERIFY(QFile::exists(attachments.first().filePath));
}

void TestChatShell::pasteWebUrlInlinesText() {
    const qint64 sid = Persistence::instance().createSession(
        std::nullopt, QStringLiteral("paste-url"), QDir::currentPath());

    PublicChatInput w;
    w.setCurrentSessionId(sid);

    QMimeData md;
    md.setUrls({QUrl(QStringLiteral("https://example.com/a")),
                QUrl(QStringLiteral("https://example.com/b"))});
    w.publicInsertFromMimeData(&md);

    const QString text = w.toPlainText();
    QVERIFY(text.contains(QStringLiteral("https://example.com/a")));
    QVERIFY(text.contains(QStringLiteral("https://example.com/b")));
}

void TestChatShell::pasteFileUrlRejects() {
    const qint64 sid = Persistence::instance().createSession(
        std::nullopt, QStringLiteral("paste-file"), QDir::currentPath());

    PublicChatInput w;
    w.setCurrentSessionId(sid);

    QSignalSpy spy(&w, &ChatInputWidget::chatStatusMessage);
    QMimeData md;
    md.setUrls({QUrl::fromLocalFile(QStringLiteral("/etc/hostname"))});
    w.publicInsertFromMimeData(&md);

    QCOMPARE(spy.count(), 1);
    QVERIFY(!w.toPlainText().contains(QStringLiteral("/etc/hostname")));
}

void TestChatShell::pasteImageRejects() {
    const qint64 sid = Persistence::instance().createSession(
        std::nullopt, QStringLiteral("paste-image"), QDir::currentPath());

    PublicChatInput w;
    w.setCurrentSessionId(sid);

    QSignalSpy spy(&w, &ChatInputWidget::chatStatusMessage);
    QMimeData md;
    QImage img(4, 4, QImage::Format_RGB32);
    img.fill(Qt::red);
    md.setImageData(img);
    w.publicInsertFromMimeData(&md);

    QCOMPARE(spy.count(), 1);
}

// --- MessageWindowWidget -----------------------------------------

void TestChatShell::messageWindowRendersAppendedMessages() {
    const qint64 sid = Persistence::instance().createSession(
        std::nullopt, QStringLiteral("render-test"), QDir::currentPath());

    MessageWindowWidget mw;
    mw.setCurrentSessionId(sid);
    mw.resize(400, 300);
    QVERIFY(mw.messageView()->toPlainText().isEmpty());

    Persistence::instance().appendMessageAndClearDraft(
        sid, QStringLiteral("user"), QByteArray("first message"));
    Persistence::instance().appendMessageAndClearDraft(
        sid, QStringLiteral("assistant"), QByteArray("second message"));

    // Process queued signals so messagesAppended slot runs.
    QCoreApplication::processEvents();

    const QString rendered = mw.messageView()->toPlainText();
    QVERIFY(rendered.contains(QStringLiteral("user: first message")));
    QVERIFY(rendered.contains(QStringLiteral("assistant: second message")));
}

QTEST_MAIN(TestChatShell)
#include "test_chatshell.moc"
