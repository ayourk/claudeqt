// SPDX-License-Identifier: GPL-3.0-only
//
// Unit tests for MainWindow. Coverage: minimum size, central
// widget shape, menu bar, toggle slots, plus:
//   - geometry persistence round-trip via settings_kv (load →
//     construct MainWindow → verify applied)
//   - editor.visible / left_pane.visible written on toggle
//   - right-click reset slots overwrite persisted value
//   - sub-minimum persisted size is clamped on show()
//
// Runs under QT_QPA_PLATFORM=offscreen (set by the ctest
// ENVIRONMENT property in tests/CMakeLists.txt). initTestCase
// sets a QTemporaryDir-backed DB so we don't thrash the user's
// real ~/.local/share/claudeqt/db.sqlite.

#include "mainwindow.h"
#include "persistence.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QMenuBar>
#include <QSignalSpy>
#include <QSplitter>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QWidget>
#include <QtTest/QtTest>

class TestMainWindow : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

    void minimumSizeIsEnforced();
    void centralWidgetIsOuterSplitter();
    void menuBarHasStandardMenus();
    void toggleEditorShowsAndHidesPane();
    void toggleLeftPaneRestoresSavedWidth();

    // Geometry persistence
    void loadAppliesPersistedWindowSize();
    void toggleEditorWritesEditorVisibleKey();
    void toggleLeftPaneWritesVisibilityKey();
    void resetLeftPaneWidthOverwritesKey();
    void resetTopMessageRatioOverwritesKey();
    void subMinimumPersistedSizeClampedOnShow();

private:
    // Wipe every geometry key before a test that needs a fresh
    // slate. Not a Qt slot — QtTest treats private slots as test
    // methods and would auto-invoke it. Called explicitly at the
    // top of each test that cares.
    void cleanupKeys() {
        for (const auto& k : geometryKeys()) {
            Persistence::instance().clearSetting(k);
        }
    }

    QTemporaryDir m_tmp;

    static const QStringList& geometryKeys() {
        static const QStringList keys = {
            QStringLiteral("window.width"),
            QStringLiteral("window.height"),
            QStringLiteral("window.x"),
            QStringLiteral("window.y"),
            QStringLiteral("window.maximized"),
            QStringLiteral("left_pane.width"),
            QStringLiteral("left_pane.visible"),
            QStringLiteral("top_splitter.message_ratio"),
            QStringLiteral("editor.visible"),
        };
        return keys;
    }

};

void TestMainWindow::initTestCase() {
    QVERIFY(m_tmp.isValid());
    const QString dbPath = m_tmp.filePath(QStringLiteral("mw.sqlite"));
    Persistence::setDatabasePathForTesting(dbPath);
    QCoreApplication::setApplicationName(QStringLiteral("mainwindow_test"));
    (void) Persistence::instance();
    QVERIFY(QFile::exists(dbPath));
}

// --- Layout tests ------------------------------------------------

void TestMainWindow::minimumSizeIsEnforced() {
    cleanupKeys();
    MainWindow w;
    QCOMPARE(w.minimumWidth(), MainWindow::kMinWidth);
    QCOMPARE(w.minimumHeight(), MainWindow::kMinHeight);

    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    w.resize(100, 100);
    QVERIFY(w.width() >= MainWindow::kMinWidth);
    QVERIFY(w.height() >= MainWindow::kMinHeight);
}

void TestMainWindow::centralWidgetIsOuterSplitter() {
    cleanupKeys();
    MainWindow w;
    auto* splitter = qobject_cast<QSplitter*>(w.centralWidget());
    QVERIFY(splitter != nullptr);
    QCOMPARE(splitter->orientation(), Qt::Horizontal);
    QCOMPARE(splitter->count(), 2);
    QCOMPARE(splitter, w.outerSplitter());

    QVERIFY(w.leftPane() != nullptr);
    QCOMPARE(w.leftPane()->parent(), splitter);
    QVERIFY(w.topSplitter() != nullptr);
}

void TestMainWindow::menuBarHasStandardMenus() {
    cleanupKeys();
    MainWindow w;
    auto* mb = w.menuBar();
    QVERIFY(mb != nullptr);

    QStringList titles;
    for (QAction* a : mb->actions()) {
        titles << a->text().remove(QLatin1Char('&'));
    }
    QVERIFY(titles.contains(QStringLiteral("File")));
    QVERIFY(titles.contains(QStringLiteral("Edit")));
    QVERIFY(titles.contains(QStringLiteral("View")));
    QVERIFY(titles.contains(QStringLiteral("Session")));
    QVERIFY(titles.contains(QStringLiteral("Help")));
}

void TestMainWindow::toggleEditorShowsAndHidesPane() {
    cleanupKeys();
    MainWindow w;
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));

    QVERIFY(!w.editorPane()->isVisible());

    w.toggleEditor();
    QVERIFY(w.editorPane()->isVisible());
    const auto sizesOn = w.topSplitter()->sizes();
    QCOMPARE(sizesOn.size(), 2);
    QVERIFY(sizesOn[0] > 0);
    QVERIFY(sizesOn[1] > 0);

    w.toggleEditor();
    QVERIFY(!w.editorPane()->isVisible());
}

void TestMainWindow::toggleLeftPaneRestoresSavedWidth() {
    cleanupKeys();
    MainWindow w;
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));

    QVERIFY(w.leftPane()->isVisible());
    const int beforeWidth = w.leftPane()->width();
    QVERIFY(beforeWidth > 0);

    w.toggleLeftPane();
    QVERIFY(!w.leftPane()->isVisible());

    w.toggleLeftPane();
    QVERIFY(w.leftPane()->isVisible());
    QVERIFY(qAbs(w.leftPane()->width() - beforeWidth) <= 5);
}

// --- Geometry persistence tests ---------------------------------

void TestMainWindow::loadAppliesPersistedWindowSize() {
    cleanupKeys();
    Persistence::instance().setSetting(
        QStringLiteral("window.width"), QByteArray::number(900));
    Persistence::instance().setSetting(
        QStringLiteral("window.height"), QByteArray::number(700));

    MainWindow w;
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));

    // Load path must resize the window to the persisted values
    // (both are well above the 640×400 floor so no clamp applies).
    QCOMPARE(w.width(), 900);
    QCOMPARE(w.height(), 700);
}

void TestMainWindow::toggleEditorWritesEditorVisibleKey() {
    cleanupKeys();
    MainWindow w;
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));

    w.toggleEditor();  // hidden → visible
    QCOMPARE(Persistence::instance().getSetting(
                 QStringLiteral("editor.visible")),
             QByteArray("1"));

    w.toggleEditor();  // visible → hidden
    QCOMPARE(Persistence::instance().getSetting(
                 QStringLiteral("editor.visible")),
             QByteArray("0"));
}

void TestMainWindow::toggleLeftPaneWritesVisibilityKey() {
    cleanupKeys();
    MainWindow w;
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));

    w.toggleLeftPane();  // visible → hidden
    QCOMPARE(Persistence::instance().getSetting(
                 QStringLiteral("left_pane.visible")),
             QByteArray("0"));

    w.toggleLeftPane();  // hidden → visible
    QCOMPARE(Persistence::instance().getSetting(
                 QStringLiteral("left_pane.visible")),
             QByteArray("1"));
}

void TestMainWindow::resetLeftPaneWidthOverwritesKey() {
    cleanupKeys();
    // Simulate a previous drag by seeding a non-default width.
    Persistence::instance().setSetting(
        QStringLiteral("left_pane.width"), QByteArray::number(400));

    MainWindow w;
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));

    w.resetLeftPaneWidth();
    QCOMPARE(Persistence::instance().getSetting(
                 QStringLiteral("left_pane.width")),
             QByteArray::number(MainWindow::kDefaultLeftPaneWidth));
}

void TestMainWindow::resetTopMessageRatioOverwritesKey() {
    cleanupKeys();
    MainWindow w;
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));

    w.resetTopMessageRatio();
    const auto blob = Persistence::instance().getSetting(
        QStringLiteral("top_splitter.message_ratio"));
    bool ok = false;
    const double stored = QString::fromUtf8(blob).toDouble(&ok);
    QVERIFY(ok);
    QVERIFY(qAbs(stored - MainWindow::kDefaultTopMessageRatio) < 1e-6);
}

void TestMainWindow::subMinimumPersistedSizeClampedOnShow() {
    cleanupKeys();
    // Seed a sub-minimum size (e.g., from a hand-edit of
    // settings_kv). Qt's minimumSize propagation will snap the
    // actual frame up on the first layout pass; showEvent() then
    // rewrites the persisted keys to the clamped value.
    Persistence::instance().setSetting(
        QStringLiteral("window.width"), QByteArray::number(200));
    Persistence::instance().setSetting(
        QStringLiteral("window.height"), QByteArray::number(200));

    MainWindow w;
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));

    QVERIFY(w.width() >= MainWindow::kMinWidth);
    QVERIFY(w.height() >= MainWindow::kMinHeight);

    // And the persisted values have been rewritten to the clamped
    // actual frame size, not left at 200.
    const int storedW = QString::fromUtf8(
        Persistence::instance().getSetting(
            QStringLiteral("window.width"))).toInt();
    const int storedH = QString::fromUtf8(
        Persistence::instance().getSetting(
            QStringLiteral("window.height"))).toInt();
    QVERIFY(storedW >= MainWindow::kMinWidth);
    QVERIFY(storedH >= MainWindow::kMinHeight);
}

QTEST_MAIN(TestMainWindow)
#include "test_mainwindow.moc"
