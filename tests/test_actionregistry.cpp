// SPDX-License-Identifier: GPL-3.0-only
//
// ActionRegistry singleton — covers registration, lookup,
// duplicate rejection, ordering, and default-shortcut metadata.
// Also asserts that MainWindow::buildMenuBar populates the
// registry with the canonical file/edit/view/session/help ids —
// catches regressions where a future refactor goes back to
// ad-hoc `new QAction(...)` and silently bypasses the registry.

#include "actionregistry.h"
#include "mainwindow.h"

#include <QAction>
#include <QKeySequence>
#include <QSignalSpy>
#include <QtTest>

class TestActionRegistry : public QObject {
    Q_OBJECT
private slots:
    void init();
    void registerAndLookup();
    void duplicateIdRejected();
    void emptyIdRejected();
    void orderStable();
    void defaultShortcutMetadata();
    void signalEmitted();
    void mainWindowPopulatesCanonicalIds();
};

void TestActionRegistry::init() {
    ActionRegistry::instance().clearForTesting();
}

void TestActionRegistry::registerAndLookup() {
    ActionRegistry& reg = ActionRegistry::instance();
    QAction* act = reg.registerAction(
        QStringLiteral("test.hello"), QStringLiteral("Hello"),
        QKeySequence(QStringLiteral("Ctrl+H")));
    QVERIFY(act != nullptr);
    QCOMPARE(act->text(), QStringLiteral("Hello"));
    QCOMPARE(act->shortcut(), QKeySequence(QStringLiteral("Ctrl+H")));

    QVERIFY(reg.contains(QStringLiteral("test.hello")));
    QCOMPARE(reg.action(QStringLiteral("test.hello")), act);
}

void TestActionRegistry::duplicateIdRejected() {
    ActionRegistry& reg = ActionRegistry::instance();
    QAction* first = reg.registerAction(
        QStringLiteral("test.dup"), QStringLiteral("First"));
    QVERIFY(first != nullptr);

    // Suppress the expected qWarning so the test output stays clean.
    QTest::ignoreMessage(QtWarningMsg,
        QRegularExpression(QStringLiteral("duplicate id")));
    QAction* second = reg.registerAction(
        QStringLiteral("test.dup"), QStringLiteral("Second"));
    QCOMPARE(second, nullptr);

    // Original action is still the one reg.action returns.
    QCOMPARE(reg.action(QStringLiteral("test.dup")), first);
    QCOMPARE(first->text(), QStringLiteral("First"));
}

void TestActionRegistry::emptyIdRejected() {
    ActionRegistry& reg = ActionRegistry::instance();
    QTest::ignoreMessage(QtWarningMsg,
        QRegularExpression(QStringLiteral("empty id")));
    QAction* act = reg.registerAction(
        QString(), QStringLiteral("Nameless"));
    QCOMPARE(act, nullptr);
    QVERIFY(reg.actionIds().isEmpty());
}

void TestActionRegistry::orderStable() {
    ActionRegistry& reg = ActionRegistry::instance();
    reg.registerAction(QStringLiteral("a.one"), QStringLiteral("One"));
    reg.registerAction(QStringLiteral("b.two"), QStringLiteral("Two"));
    reg.registerAction(QStringLiteral("c.three"), QStringLiteral("Three"));

    QList<QString> ids = reg.actionIds();
    QCOMPARE(ids.size(), 3);
    QCOMPARE(ids[0], QStringLiteral("a.one"));
    QCOMPARE(ids[1], QStringLiteral("b.two"));
    QCOMPARE(ids[2], QStringLiteral("c.three"));
}

void TestActionRegistry::defaultShortcutMetadata() {
    ActionRegistry& reg = ActionRegistry::instance();
    const QKeySequence seq(QStringLiteral("Ctrl+Shift+F"));
    QAction* act = reg.registerAction(
        QStringLiteral("test.find"), QStringLiteral("Find"), seq);
    QVERIFY(act != nullptr);

    QCOMPARE(reg.defaultShortcut(QStringLiteral("test.find")), seq);

    // Simulate a future KeyBindingsDialog overwriting the live
    // shortcut; the default should remain untouched for the
    // "reset to default" button.
    act->setShortcut(QKeySequence(QStringLiteral("F3")));
    QCOMPARE(reg.defaultShortcut(QStringLiteral("test.find")), seq);
    QCOMPARE(reg.action(QStringLiteral("test.find"))->shortcut(),
             QKeySequence(QStringLiteral("F3")));
}

void TestActionRegistry::signalEmitted() {
    ActionRegistry& reg = ActionRegistry::instance();
    QSignalSpy spy(&reg, &ActionRegistry::actionRegistered);
    reg.registerAction(QStringLiteral("test.signal"),
                       QStringLiteral("Signal"));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().first().toString(), QStringLiteral("test.signal"));
}

void TestActionRegistry::mainWindowPopulatesCanonicalIds() {
    // Canonical menu action ids from the §12 DoD (line 3098). These
    // must be registered by the time MainWindow finishes
    // construction. Any future refactor that reverts a menu entry
    // to a direct `new QAction` breaks this invariant.
    const QStringList expected = {
        QStringLiteral("file.new_project"),
        QStringLiteral("file.new_session"),
        QStringLiteral("file.open_file"),
        QStringLiteral("file.save"),
        QStringLiteral("file.save_as"),
        QStringLiteral("file.close_session"),
        QStringLiteral("file.quit"),
        QStringLiteral("edit.undo"),
        QStringLiteral("edit.redo"),
        QStringLiteral("edit.cut"),
        QStringLiteral("edit.copy"),
        QStringLiteral("edit.paste"),
        QStringLiteral("edit.preferences"),
        QStringLiteral("view.toggle_editor"),
        QStringLiteral("view.toggle_left_pane"),
        QStringLiteral("view.reset_window_layout"),
        QStringLiteral("session.rename"),
        QStringLiteral("session.change_cwd"),
        QStringLiteral("session.move_to_project"),
        QStringLiteral("session.delete"),
        QStringLiteral("help.about"),
        QStringLiteral("help.github"),
    };

    MainWindow w;

    ActionRegistry& reg = ActionRegistry::instance();
    for (const QString& id : expected) {
        QVERIFY2(reg.contains(id),
                 qPrintable(QStringLiteral(
                     "MainWindow did not register action id: %1").arg(id)));
        QVERIFY(reg.action(id) != nullptr);
    }
}

QTEST_MAIN(TestActionRegistry)
#include "test_actionregistry.moc"
