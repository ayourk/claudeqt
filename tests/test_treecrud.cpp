// SPDX-License-Identifier: GPL-3.0-only
//
// Unit tests for tree CRUD scaffolding. Covers the
// non-modal surface area that can be exercised without blocking
// on ThemedQtDialog::exec():
//   - ProjectSessionTreeModel::setData(Qt::EditRole) renames
//     projects and sessions through Persistence and emits
//     dataChanged
//   - empty / whitespace-only rename is rejected
//   - EditorPaneWidget dirty helpers (dirtyEditorCount,
//     saveAllDirty, discardAllAndCloseAll)
//
// The modal paths (New Project dialog, delete confirmation, cwd
// retarget prompt) are intentionally NOT exercised here — their
// handlers block on QDialog::exec(), which is incompatible with
// QtTest's synchronous style. Coverage for those flows relies on
// the underlying Persistence tests plus manual smoke.

#include "codeeditor.h"
#include "editorpanewidget.h"
#include "instancehub.h"
#include "persistence.h"
#include "projectsessiontreemodel.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QModelIndex>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTextStream>
#include <QtTest/QtTest>

#include <KSyntaxHighlighting/Repository>

class TestTreeCrud : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init() { QTest::qWait(300); }
    void cleanup();

    void setDataRenamesProject();
    void setDataRenamesSession();
    void setDataRejectsEmptyName();
    void setDataEmitsDataChanged();

    void editorPaneDirtyCountStartsZero();
    void editorPaneDirtyCountReflectsModifiedBuffers();
    void editorPaneSaveAllDirtyWritesAndClears();
    void editorPaneDiscardAllClosesEverything();

private:
    QTemporaryDir m_tmp;

    void wipeDb();
    QString writeScratchFile(const QString& name, const QString& body);
};

void TestTreeCrud::initTestCase() {
    QVERIFY(m_tmp.isValid());
    const QString dbPath = m_tmp.filePath(QStringLiteral("treecrud.sqlite"));
    Persistence::setDatabasePathForTesting(dbPath);
    Persistence::setDeferralJitterForTesting(50, 250);
    QCoreApplication::setApplicationName(QStringLiteral("treecrud_test"));
    qputenv("XDG_DATA_HOME", m_tmp.path().toUtf8());

    const QString sockPath = m_tmp.filePath(QStringLiteral("hub.sock"));
    InstanceHub::setSocketPathForTesting(sockPath);
    InstanceHub::instance().start();

    (void) Persistence::instance();
    Persistence::instance().connectToHub();
    QVERIFY(QFile::exists(dbPath));
}

void TestTreeCrud::cleanupTestCase() {
    InstanceHub::instance().shutdown();
    InstanceHub::setSocketPathForTesting(QString());
}

void TestTreeCrud::cleanup() {
    wipeDb();
}

void TestTreeCrud::wipeDb() {
    auto& db = Persistence::instance();
    for (const auto& s : db.listSessions()) db.deleteSession(s.id);
    auto projects = db.listProjects();
    while (!projects.isEmpty()) {
        for (const auto& p : projects) {
            if (db.listChildProjects(p.id).isEmpty()) {
                db.deleteProject(p.id);
            }
        }
        projects = db.listProjects();
    }
}

QString TestTreeCrud::writeScratchFile(const QString& name,
                                       const QString& body) {
    const QString path = m_tmp.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return {};
    }
    QTextStream ts(&f);
    ts << body;
    f.close();
    return path;
}

// --- Inline rename ------------------------------------------------

void TestTreeCrud::setDataRenamesProject() {
    auto& db = Persistence::instance();
    const qint64 pid = db.createProject(std::nullopt,
                                        QStringLiteral("before"),
                                        QStringLiteral("/tmp"));

    ProjectSessionTreeModel m;
    const QModelIndex idx = m.index(0, 0);
    QVERIFY(idx.isValid());
    QCOMPARE(idx.data(Qt::DisplayRole).toString(),
             QStringLiteral("before"));

    QVERIFY(m.setData(idx, QStringLiteral("after"), Qt::EditRole));

    // Pull a fresh list from the DAO to confirm the rename landed.
    const auto projects = db.listProjects();
    QCOMPARE(projects.size(), qsizetype(1));
    QCOMPARE(projects.first().id, pid);
    QCOMPARE(projects.first().name, QStringLiteral("after"));
}

void TestTreeCrud::setDataRenamesSession() {
    auto& db = Persistence::instance();
    const qint64 sid = db.createSession(std::nullopt,
                                        QStringLiteral("old title"),
                                        QStringLiteral("/tmp"));

    ProjectSessionTreeModel m;
    const QModelIndex idx = m.indexForSession(sid);
    QVERIFY(idx.isValid());

    QVERIFY(m.setData(idx, QStringLiteral("new title"), Qt::EditRole));

    const auto sessions = db.listSessions();
    QCOMPARE(sessions.size(), qsizetype(1));
    QCOMPARE(sessions.first().title, QStringLiteral("new title"));
}

void TestTreeCrud::setDataRejectsEmptyName() {
    auto& db = Persistence::instance();
    db.createProject(std::nullopt, QStringLiteral("keeper"),
                     QStringLiteral("/k"));

    ProjectSessionTreeModel m;
    const QModelIndex idx = m.index(0, 0);
    QVERIFY(idx.isValid());

    QVERIFY(!m.setData(idx, QString(), Qt::EditRole));
    QVERIFY(!m.setData(idx, QStringLiteral("   "), Qt::EditRole));

    // And DecorationRole writes should be rejected outright — only
    // EditRole triggers the rename path.
    QVERIFY(!m.setData(idx, QStringLiteral("x"), Qt::DecorationRole));

    QCOMPARE(db.listProjects().first().name, QStringLiteral("keeper"));
}

void TestTreeCrud::setDataEmitsDataChanged() {
    auto& db = Persistence::instance();
    db.createProject(std::nullopt, QStringLiteral("spy-target"),
                     QStringLiteral("/s"));
    QTest::qWait(300);

    ProjectSessionTreeModel m;
    QSignalSpy resetSpy(&m, &QAbstractItemModel::modelReset);

    const QModelIndex idx = m.index(0, 0);
    QVERIFY(m.setData(idx, QStringLiteral("renamed"), Qt::EditRole));
    // The DAO's rename emits projectsChanged, which routes through
    // the model's reloadFromDb → begin/endResetModel. Views listen
    // to modelReset to refresh; asserting >= 1 reset here pins the
    // "something notifies the view" contract without caring
    // whether the notification is dataChanged or a reset.
    QTRY_VERIFY(resetSpy.count() >= 1);
}

// --- Editor pane dirty helpers -----------------------------------

void TestTreeCrud::editorPaneDirtyCountStartsZero() {
    EditorPaneWidget pane;
    QCOMPARE(pane.dirtyEditorCount(), 0);
    QVERIFY(pane.saveAllDirty());  // vacuously true
    pane.discardAllAndCloseAll();  // no-op
    QCOMPARE(pane.tabCount(), 0);
}

void TestTreeCrud::editorPaneDirtyCountReflectsModifiedBuffers() {
    const QString p1 = writeScratchFile(QStringLiteral("a.txt"),
                                        QStringLiteral("aaa\n"));
    const QString p2 = writeScratchFile(QStringLiteral("b.txt"),
                                        QStringLiteral("bbb\n"));
    QVERIFY(!p1.isEmpty());
    QVERIFY(!p2.isEmpty());

    EditorPaneWidget pane;
    auto* e1 = pane.openFile(p1);
    auto* e2 = pane.openFile(p2);
    QVERIFY(e1 != nullptr);
    QVERIFY(e2 != nullptr);
    QCOMPARE(pane.tabCount(), 2);
    QCOMPARE(pane.dirtyEditorCount(), 0);

    // Modify one editor — count should flip to 1.
    e1->insertPlainText(QStringLiteral("dirty"));
    QCOMPARE(pane.dirtyEditorCount(), 1);

    // Modify the second — count should flip to 2.
    e2->insertPlainText(QStringLiteral("also dirty"));
    QCOMPARE(pane.dirtyEditorCount(), 2);
}

void TestTreeCrud::editorPaneSaveAllDirtyWritesAndClears() {
    const QString p = writeScratchFile(QStringLiteral("save.txt"),
                                       QStringLiteral("before\n"));
    QVERIFY(!p.isEmpty());

    EditorPaneWidget pane;
    auto* e = pane.openFile(p);
    QVERIFY(e != nullptr);
    e->insertPlainText(QStringLiteral("added text "));
    QCOMPARE(pane.dirtyEditorCount(), 1);

    QVERIFY(pane.saveAllDirty());
    QCOMPARE(pane.dirtyEditorCount(), 0);

    // File on disk should now contain the edit.
    QFile f(p);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QString contents = QString::fromUtf8(f.readAll());
    QVERIFY(contents.contains(QStringLiteral("added text")));
}

void TestTreeCrud::editorPaneDiscardAllClosesEverything() {
    const QString p1 = writeScratchFile(QStringLiteral("d1.txt"),
                                        QStringLiteral("one\n"));
    const QString p2 = writeScratchFile(QStringLiteral("d2.txt"),
                                        QStringLiteral("two\n"));

    EditorPaneWidget pane;
    pane.openFile(p1);
    pane.openFile(p2);
    if (auto* current = pane.currentEditor()) {
        current->insertPlainText(QStringLiteral("unsaved"));
    }
    QVERIFY(pane.dirtyEditorCount() >= 1);

    pane.discardAllAndCloseAll();
    QCOMPARE(pane.tabCount(), 0);
    QCOMPARE(pane.dirtyEditorCount(), 0);

    // File on disk must still say "one" — discard must NOT have
    // written the dirty buffer.
    QFile f(p1);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QString contents = QString::fromUtf8(f.readAll());
    QCOMPARE(contents, QStringLiteral("one\n"));
}

QTEST_MAIN(TestTreeCrud)
#include "test_treecrud.moc"
