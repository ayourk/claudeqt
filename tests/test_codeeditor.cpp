// SPDX-License-Identifier: GPL-3.0-only
//
// Unit tests for CodeEditor + EditorPaneWidget. Covers the KSH
// repository construction, dirty tracking via modificationChanged,
// save roundtrip, load-from-file, line-wrap-mode toggling, tab
// open/close, dedup of already-open files, and the clean-reload
// path of external-modification detection. The dirty-buffer
// external-mod prompt is deferred to a follow-up.
//
// Runs offscreen via QT_QPA_PLATFORM set in tests/CMakeLists.txt.

#include "codeeditor.h"
#include "editorpanewidget.h"
#include "persistence.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QPlainTextEdit>
#include <QString>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>
#include <QTextDocument>
#include <QtTest/QtTest>

class TestCodeEditor : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

    // CodeEditor — I/O
    void loadFromFileReadsBytes();
    void loadFromFileMissingReturnsFalse();
    void saveRoundtripMatchesDisk();
    void saveEmptyPathFails();

    // CodeEditor — dirty tracking
    void typingFlipsModified();
    void saveClearsDirty();

    // CodeEditor — line wrap mode
    void defaultLineWrapIsNoWrap();
    void setLineWrapModeApplies();

    // EditorPaneWidget
    void openFileAddsTab();
    void openFileDedupsExistingTab();
    void closeTabRemovesEditor();
    void saveCurrentDelegatesToEditor();
    void globalWrapModeAppliesToExistingEditors();

    // External modification — clean reload path
    void externalCleanReloadUpdatesDocument();

private:
    QTemporaryDir m_tmp;
    QString makeFile(const QString& name, const QByteArray& bytes) {
        const QString path = m_tmp.filePath(name);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) {
            qFatal("makeFile: cannot open %s", qPrintable(path));
        }
        f.write(bytes);
        f.close();
        return path;
    }
};

void TestCodeEditor::initTestCase() {
    QVERIFY(m_tmp.isValid());
    const QString dbPath = m_tmp.filePath(QStringLiteral("ce.sqlite"));
    Persistence::setDatabasePathForTesting(dbPath);
    QCoreApplication::setApplicationName(
        QStringLiteral("codeeditor_test"));
    (void) Persistence::instance();
}

// --- CodeEditor I/O ---------------------------------------------

void TestCodeEditor::loadFromFileReadsBytes() {
    EditorPaneWidget pane;
    auto* ed = new CodeEditor(pane.repository(), &pane);
    const QString path = makeFile(
        QStringLiteral("a.txt"),
        QByteArrayLiteral("hello world\nsecond line\n"));

    QVERIFY(ed->loadFromFile(path));
    QCOMPARE(ed->filePath(), path);
    QCOMPARE(ed->toPlainText(),
             QStringLiteral("hello world\nsecond line\n"));
    QVERIFY(!ed->document()->isModified());
    QVERIFY(ed->savedMtime() > 0);
    delete ed;
}

void TestCodeEditor::loadFromFileMissingReturnsFalse() {
    EditorPaneWidget pane;
    auto* ed = new CodeEditor(pane.repository(), &pane);
    const QString bogus = m_tmp.filePath(QStringLiteral("does-not-exist.txt"));
    QVERIFY(!ed->loadFromFile(bogus));
    QVERIFY(ed->toPlainText().isEmpty());
    delete ed;
}

void TestCodeEditor::saveRoundtripMatchesDisk() {
    EditorPaneWidget pane;
    auto* ed = new CodeEditor(pane.repository(), &pane);
    const QString path = makeFile(
        QStringLiteral("b.txt"),
        QByteArrayLiteral("original\n"));

    QVERIFY(ed->loadFromFile(path));
    // setPlainText would reset the modified flag — simulate a
    // real keystroke by selecting all and replacing via cursor.
    QTextCursor cursor = ed->textCursor();
    cursor.select(QTextCursor::Document);
    cursor.insertText(QStringLiteral("mutated\n"));
    QVERIFY(ed->document()->isModified());

    QVERIFY(ed->save());
    QVERIFY(!ed->document()->isModified());

    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), QByteArrayLiteral("mutated\n"));
    delete ed;
}

void TestCodeEditor::saveEmptyPathFails() {
    EditorPaneWidget pane;
    auto* ed = new CodeEditor(pane.repository(), &pane);
    ed->setPlainText(QStringLiteral("untitled content"));
    QVERIFY(!ed->save());  // no filePath yet
    delete ed;
}

// --- CodeEditor dirty tracking ---------------------------------

void TestCodeEditor::typingFlipsModified() {
    EditorPaneWidget pane;
    auto* ed = new CodeEditor(pane.repository(), &pane);
    const QString path = makeFile(
        QStringLiteral("c.txt"), QByteArrayLiteral("x\n"));
    QVERIFY(ed->loadFromFile(path));
    QVERIFY(!ed->document()->isModified());

    QSignalSpy spy(ed, &CodeEditor::dirtyStateChanged);
    ed->textCursor().insertText(QStringLiteral("y"));
    QVERIFY(ed->document()->isModified());
    QVERIFY(spy.count() >= 1);
    QCOMPARE(spy.last().at(0).toBool(), true);
    delete ed;
}

void TestCodeEditor::saveClearsDirty() {
    EditorPaneWidget pane;
    auto* ed = new CodeEditor(pane.repository(), &pane);
    const QString path = makeFile(
        QStringLiteral("d.txt"), QByteArrayLiteral("x\n"));
    QVERIFY(ed->loadFromFile(path));
    QTextCursor cursor = ed->textCursor();
    cursor.select(QTextCursor::Document);
    cursor.insertText(QStringLiteral("dirty\n"));
    QVERIFY(ed->document()->isModified());

    QSignalSpy spy(ed, &CodeEditor::dirtyStateChanged);
    QVERIFY(ed->save());
    QVERIFY(!ed->document()->isModified());
    // At least one "false" signal after save.
    bool sawFalse = false;
    for (const auto& args : spy) {
        if (!args.at(0).toBool()) sawFalse = true;
    }
    QVERIFY(sawFalse);
    delete ed;
}

// --- Line wrap --------------------------------------------------

void TestCodeEditor::defaultLineWrapIsNoWrap() {
    EditorPaneWidget pane;
    auto* ed = new CodeEditor(pane.repository(), &pane);
    QCOMPARE(ed->lineWrapMode(), QPlainTextEdit::NoWrap);
    delete ed;
}

void TestCodeEditor::setLineWrapModeApplies() {
    EditorPaneWidget pane;
    auto* ed = new CodeEditor(pane.repository(), &pane);
    ed->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    QCOMPARE(ed->lineWrapMode(), QPlainTextEdit::WidgetWidth);
    delete ed;
}

// --- EditorPaneWidget ------------------------------------------

void TestCodeEditor::openFileAddsTab() {
    EditorPaneWidget pane;
    QCOMPARE(pane.tabCount(), 0);

    const QString path = makeFile(
        QStringLiteral("pane-a.txt"),
        QByteArrayLiteral("alpha\n"));
    auto* ed = pane.openFile(path);
    QVERIFY(ed != nullptr);
    QCOMPARE(pane.tabCount(), 1);
    QCOMPARE(pane.currentEditor(), ed);
    QCOMPARE(ed->toPlainText(), QStringLiteral("alpha\n"));
}

void TestCodeEditor::openFileDedupsExistingTab() {
    EditorPaneWidget pane;
    const QString path = makeFile(
        QStringLiteral("pane-b.txt"),
        QByteArrayLiteral("beta\n"));

    auto* first = pane.openFile(path);
    QCOMPARE(pane.tabCount(), 1);
    auto* again = pane.openFile(path);
    QCOMPARE(pane.tabCount(), 1);
    QCOMPARE(first, again);
}

void TestCodeEditor::closeTabRemovesEditor() {
    EditorPaneWidget pane;
    const QString path = makeFile(
        QStringLiteral("pane-c.txt"),
        QByteArrayLiteral("gamma\n"));
    QVERIFY(pane.openFile(path) != nullptr);
    QCOMPARE(pane.tabCount(), 1);

    pane.closeTab(0);
    QCOMPARE(pane.tabCount(), 0);
    QVERIFY(pane.currentEditor() == nullptr);
}

void TestCodeEditor::saveCurrentDelegatesToEditor() {
    EditorPaneWidget pane;
    const QString path = makeFile(
        QStringLiteral("pane-d.txt"),
        QByteArrayLiteral("old\n"));
    auto* ed = pane.openFile(path);
    QVERIFY(ed != nullptr);
    QTextCursor cursor = ed->textCursor();
    cursor.select(QTextCursor::Document);
    cursor.insertText(QStringLiteral("new\n"));
    QVERIFY(ed->document()->isModified());
    QVERIFY(pane.saveCurrent());
    QVERIFY(!ed->document()->isModified());

    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), QByteArrayLiteral("new\n"));
}

void TestCodeEditor::globalWrapModeAppliesToExistingEditors() {
    EditorPaneWidget pane;
    const QString p1 = makeFile(QStringLiteral("w1.txt"),
                                QByteArrayLiteral("1\n"));
    const QString p2 = makeFile(QStringLiteral("w2.txt"),
                                QByteArrayLiteral("2\n"));
    auto* e1 = pane.openFile(p1);
    auto* e2 = pane.openFile(p2);
    QVERIFY(e1 && e2);
    QCOMPARE(e1->lineWrapMode(), QPlainTextEdit::NoWrap);

    pane.setGlobalLineWrapMode(QPlainTextEdit::WidgetWidth);
    QCOMPARE(e1->lineWrapMode(), QPlainTextEdit::WidgetWidth);
    QCOMPARE(e2->lineWrapMode(), QPlainTextEdit::WidgetWidth);
    QCOMPARE(pane.globalLineWrapMode(), QPlainTextEdit::WidgetWidth);
}

// --- External modification (clean reload) -----------------------

void TestCodeEditor::externalCleanReloadUpdatesDocument() {
    EditorPaneWidget pane;
    auto* ed = new CodeEditor(pane.repository(), &pane);
    const QString path = makeFile(
        QStringLiteral("ext.txt"),
        QByteArrayLiteral("before\n"));
    QVERIFY(ed->loadFromFile(path));
    QVERIFY(!ed->document()->isModified());
    const qint64 originalMtime = ed->savedMtime();

    // Force a mtime bump by waiting past filesystem mtime
    // granularity (1 s on ext4 / most tmpfs) and rewriting.
    QTest::qWait(1100);
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QByteArrayLiteral("after\n"));
        f.close();
    }
    const qint64 diskMtime =
        QFileInfo(path).lastModified().toSecsSinceEpoch();
    QVERIFY(diskMtime > originalMtime);

    // Trigger the focus-in external-mod check directly. We
    // can't reliably deliver a QFocusEvent under offscreen, so
    // we call the public contract: loadFromFile re-reads and
    // is the same path the clean-reload branch takes.
    QFocusEvent fe(QEvent::FocusIn);
    QCoreApplication::sendEvent(ed, &fe);

    QCOMPARE(ed->toPlainText(), QStringLiteral("after\n"));
    QCOMPARE(ed->savedMtime(), diskMtime);
    delete ed;
}

QTEST_MAIN(TestCodeEditor)
#include "test_codeeditor.moc"
