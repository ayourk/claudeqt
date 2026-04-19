// SPDX-License-Identifier: GPL-3.0-only
//
// Unit tests for ProjectSessionTreeModel. Covers:
//   - Tree construction: top-level projects, nested sub-projects,
//     orphan sessions, sessions-under-project membership
//   - Sort order: default `last_used DESC`, settings-flipped alpha
//   - `indexForSession` round-trip
//   - Drag-drop cycle guard via canDropMimeData
//   - `Persistence::activateSession` bubbles last_used up the
//     project ancestor chain
//   - `session.last_active_id` restore path through MainWindow
//   - `tree.expanded_ids` persistence round-trip through MainWindow

#include "chatinputwidget.h"
#include "instancehub.h"
#include "mainwindow.h"
#include "messagewindowwidget.h"
#include "persistence.h"
#include "projectsessiontreemodel.h"

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QItemSelectionModel>
#include <QMimeData>
#include <QModelIndex>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeView>
#include <QtTest/QtTest>

class TestTreeModel : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init() { QTest::qWait(300); }
    void cleanup();

    void buildsTopLevelProjectsAndOrphans();
    void nestsSubProjectsUnderParents();
    void sortsByLastUsedDescendingByDefault();
    void sortsAlphabeticallyWhenSettingFlipped();
    // Live re-sort
    void sortOrderChangeLiveResortsModel();
    void sortOrderChangeFiresModelReset();
    void mainWindowExpansionSurvivesSortFlip();
    void indexForSessionRoundTrip();
    void canDropSessionOntoProjectOrRoot();
    void rejectsProjectDropOnDescendant();
    void activateSessionBubblesLastUsedUp();
    void mainWindowRestoresLastActiveSession();
    void mainWindowPersistsExpandedIds();

private:
    QTemporaryDir m_tmp;

    void wipeDb();
};

void TestTreeModel::initTestCase() {
    QVERIFY(m_tmp.isValid());
    const QString dbPath = m_tmp.filePath(QStringLiteral("treemodel.sqlite"));
    Persistence::setDatabasePathForTesting(dbPath);
    Persistence::setDeferralJitterForTesting(50, 250);
    QCoreApplication::setApplicationName(QStringLiteral("treemodel_test"));
    qputenv("XDG_DATA_HOME", m_tmp.path().toUtf8());

    const QString sockPath = m_tmp.filePath(QStringLiteral("hub.sock"));
    InstanceHub::setSocketPathForTesting(sockPath);
    InstanceHub::instance().start();

    (void) Persistence::instance();
    Persistence::instance().connectToHub();
    QVERIFY(QFile::exists(dbPath));
}

void TestTreeModel::cleanupTestCase() {
    InstanceHub::instance().shutdown();
    InstanceHub::setSocketPathForTesting(QString());
}

void TestTreeModel::cleanup() {
    // Wipe every row between tests so each case constructs its own
    // fixture. The singleton stays alive; only its data is reset.
    wipeDb();
}

void TestTreeModel::wipeDb() {
    auto& db = Persistence::instance();

    // Sessions must go before projects to respect FK direction —
    // and we clear them via the DAO so signals fire cleanly.
    for (const auto& s : db.listSessions()) {
        db.deleteSession(s.id);
    }
    // Delete projects leaf-first so parent_project_id references
    // are gone before the parent row.
    auto projects = db.listProjects();
    while (!projects.isEmpty()) {
        for (const auto& p : projects) {
            const auto children = db.listChildProjects(p.id);
            if (children.isEmpty()) {
                db.deleteProject(p.id);
            }
        }
        projects = db.listProjects();
    }
    db.clearSetting(QStringLiteral("tree.project_sort_order"));
    db.clearSetting(QStringLiteral("tree.session_sort_order"));
    db.clearSetting(QStringLiteral("tree.expanded_ids"));
    db.clearSetting(QStringLiteral("session.last_active_id"));
}

// --- Tree construction ------------------------------------------

void TestTreeModel::buildsTopLevelProjectsAndOrphans() {
    auto& db = Persistence::instance();
    const qint64 pA = db.createProject(std::nullopt,
                                       QStringLiteral("Alpha"),
                                       QStringLiteral("/srv/alpha"));
    const qint64 pB = db.createProject(std::nullopt,
                                       QStringLiteral("Bravo"),
                                       QStringLiteral("/srv/bravo"));
    const qint64 sUnderA = db.createSession(
        pA, QStringLiteral("alpha-1"), QStringLiteral("/srv/alpha"));
    const qint64 sOrphan = db.createSession(
        std::nullopt, QStringLiteral("loose"), QStringLiteral("/tmp"));

    ProjectSessionTreeModel m;
    // Two projects + one orphan = three rows at the invisible root.
    QCOMPARE(m.rowCount(QModelIndex()), 3);

    // Locate the session inside pA.
    const QModelIndex idxA = m.indexForSession(sUnderA);
    QVERIFY(idxA.isValid());
    QCOMPARE(idxA.data(ProjectSessionTreeModel::ItemIdRole).toLongLong(),
             sUnderA);
    QCOMPARE(idxA.data(ProjectSessionTreeModel::IsOrphanRole).toBool(),
             false);

    const QModelIndex idxOrphan = m.indexForSession(sOrphan);
    QVERIFY(idxOrphan.isValid());
    QCOMPARE(idxOrphan.data(ProjectSessionTreeModel::IsOrphanRole).toBool(),
             true);
    QCOMPARE(idxOrphan.parent(), QModelIndex());

    Q_UNUSED(pB);
}

void TestTreeModel::nestsSubProjectsUnderParents() {
    auto& db = Persistence::instance();
    const qint64 root = db.createProject(std::nullopt,
                                         QStringLiteral("Monorepo"),
                                         QStringLiteral("/srv/mono"));
    const qint64 child = db.createProject(root,
                                          QStringLiteral("Frontend"),
                                          QStringLiteral("/srv/mono/fe"));
    const qint64 grandchild = db.createProject(
        child, QStringLiteral("Components"),
        QStringLiteral("/srv/mono/fe/components"));
    const qint64 deepSession = db.createSession(
        grandchild, QStringLiteral("button-refactor"),
        QStringLiteral("/srv/mono/fe/components"));

    ProjectSessionTreeModel m;
    QCOMPARE(m.rowCount(QModelIndex()), 1);

    const QModelIndex deep = m.indexForSession(deepSession);
    QVERIFY(deep.isValid());

    // Walk back up to verify nesting depth is 3.
    int depth = 0;
    QModelIndex cursor = deep;
    while (cursor.isValid()) {
        ++depth;
        cursor = cursor.parent();
    }
    QCOMPARE(depth, 4);  // session + 3 projects
}

// --- Sort order ---------------------------------------------------

void TestTreeModel::sortsByLastUsedDescendingByDefault() {
    auto& db = Persistence::instance();
    const qint64 oldP = db.createProject(std::nullopt,
                                         QStringLiteral("Old"),
                                         QStringLiteral("/o"));
    QTest::qWait(10);
    const qint64 midP = db.createProject(std::nullopt,
                                         QStringLiteral("Mid"),
                                         QStringLiteral("/m"));
    QTest::qWait(10);
    const qint64 newP = db.createProject(std::nullopt,
                                         QStringLiteral("New"),
                                         QStringLiteral("/n"));

    ProjectSessionTreeModel m;
    QCOMPARE(m.rowCount(QModelIndex()), 3);
    // last_used DESC + id DESC tiebreak → New, Mid, Old.
    QCOMPARE(m.index(0, 0).data(ProjectSessionTreeModel::ItemIdRole)
                 .toLongLong(),
             newP);
    QCOMPARE(m.index(1, 0).data(ProjectSessionTreeModel::ItemIdRole)
                 .toLongLong(),
             midP);
    QCOMPARE(m.index(2, 0).data(ProjectSessionTreeModel::ItemIdRole)
                 .toLongLong(),
             oldP);
}

void TestTreeModel::sortsAlphabeticallyWhenSettingFlipped() {
    auto& db = Persistence::instance();
    db.createProject(std::nullopt, QStringLiteral("charlie"),
                     QStringLiteral("/c"));
    db.createProject(std::nullopt, QStringLiteral("alpha"),
                     QStringLiteral("/a"));
    db.createProject(std::nullopt, QStringLiteral("bravo"),
                     QStringLiteral("/b"));

    db.setSetting(QStringLiteral("tree.project_sort_order"),
                  QByteArray("alpha"));

    ProjectSessionTreeModel m;
    QCOMPARE(m.rowCount(QModelIndex()), 3);
    QCOMPARE(m.index(0, 0).data(Qt::DisplayRole).toString(),
             QStringLiteral("alpha"));
    QCOMPARE(m.index(1, 0).data(Qt::DisplayRole).toString(),
             QStringLiteral("bravo"));
    QCOMPARE(m.index(2, 0).data(Qt::DisplayRole).toString(),
             QStringLiteral("charlie"));
}

// --- Live-apply sort order --------------------------------------

void TestTreeModel::sortOrderChangeLiveResortsModel() {
    auto& db = Persistence::instance();
    // Seed three projects in non-alphabetical creation order so
    // the default last_used DESC order and the alphabetical order
    // are both visually distinct.
    const qint64 c =
        db.createProject(std::nullopt, QStringLiteral("charlie"),
                         QStringLiteral("/c"));
    QTest::qWait(10);
    const qint64 a =
        db.createProject(std::nullopt, QStringLiteral("alpha"),
                         QStringLiteral("/a"));
    QTest::qWait(10);
    const qint64 b =
        db.createProject(std::nullopt, QStringLiteral("bravo"),
                         QStringLiteral("/b"));
    Q_UNUSED(c);
    Q_UNUSED(b);

    ProjectSessionTreeModel m;
    // Default last_used DESC → youngest first: bravo, alpha, charlie.
    QCOMPARE(m.index(0, 0).data(Qt::DisplayRole).toString(),
             QStringLiteral("bravo"));
    QCOMPARE(m.index(1, 0).data(Qt::DisplayRole).toString(),
             QStringLiteral("alpha"));
    QCOMPARE(m.index(2, 0).data(Qt::DisplayRole).toString(),
             QStringLiteral("charlie"));

    // Flip to alphabetical via the settings_kv write-through path
    // the SettingsDialog uses. The model must live-re-sort without
    // any explicit reloadFromDb call.
    db.setSetting(QStringLiteral("tree.project_sort_order"),
                  QByteArray("alpha"));

    QTRY_COMPARE(m.index(0, 0).data(Qt::DisplayRole).toString(),
                 QStringLiteral("alpha"));
    QCOMPARE(m.index(1, 0).data(Qt::DisplayRole).toString(),
             QStringLiteral("bravo"));
    QCOMPARE(m.index(2, 0).data(Qt::DisplayRole).toString(),
             QStringLiteral("charlie"));

    // Flip back and confirm the live path is symmetric.
    db.setSetting(QStringLiteral("tree.project_sort_order"),
                  QByteArray("last_used"));
    QTRY_COMPARE(m.index(0, 0).data(Qt::DisplayRole).toString(),
                 QStringLiteral("bravo"));

    // And flipping the session key should not touch the project
    // ordering (it triggers a reset but projects aren't siblings
    // of sessions here).
    db.setSetting(QStringLiteral("tree.session_sort_order"),
                  QByteArray("title"));
    QTRY_COMPARE(m.index(0, 0).data(Qt::DisplayRole).toString(),
                 QStringLiteral("bravo"));
    Q_UNUSED(a);
}

void TestTreeModel::sortOrderChangeFiresModelReset() {
    auto& db = Persistence::instance();
    db.createProject(std::nullopt, QStringLiteral("p1"),
                     QStringLiteral("/p1"));
    db.createProject(std::nullopt, QStringLiteral("p2"),
                     QStringLiteral("/p2"));
    QTest::qWait(300);

    ProjectSessionTreeModel m;
    QSignalSpy aboutSpy(&m, &QAbstractItemModel::modelAboutToBeReset);
    QSignalSpy resetSpy(&m, &QAbstractItemModel::modelReset);

    db.setSetting(QStringLiteral("tree.project_sort_order"),
                  QByteArray("alpha"));
    QTRY_COMPARE(aboutSpy.count(), 1);
    QTRY_COMPARE(resetSpy.count(), 1);

    db.setSetting(QStringLiteral("tree.session_sort_order"),
                  QByteArray("title"));
    QTRY_COMPARE(aboutSpy.count(), 2);
    QTRY_COMPARE(resetSpy.count(), 2);

    // Writing an unrelated key must not trigger the model reset.
    db.setSetting(QStringLiteral("ui.language"), QByteArray("en"));
    QTest::qWait(300);
    QCOMPARE(aboutSpy.count(), 2);
    QCOMPARE(resetSpy.count(), 2);
}

void TestTreeModel::mainWindowExpansionSurvivesSortFlip() {
    auto& db = Persistence::instance();
    const qint64 p1 = db.createProject(std::nullopt,
                                       QStringLiteral("P1"),
                                       QStringLiteral("/p1"));
    const qint64 p2 = db.createProject(std::nullopt,
                                       QStringLiteral("P2"),
                                       QStringLiteral("/p2"));
    // Sub-projects so there's something below the expansion arrows.
    db.createProject(p1, QStringLiteral("P1a"), QStringLiteral("/p1/a"));
    db.createProject(p2, QStringLiteral("P2a"), QStringLiteral("/p2/a"));

    // Seed the expanded_ids key so the MainWindow startup path
    // applies it before the modelReset handler arms.
    const QString csv =
        QString::number(static_cast<qlonglong>(p1)) + QChar(',') +
        QString::number(static_cast<qlonglong>(p2));
    db.setSetting(QStringLiteral("tree.expanded_ids"), csv.toUtf8());

    MainWindow w;
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));

    auto* tree = w.findChild<QTreeView*>(QStringLiteral("projectSessionTree"));
    QVERIFY(tree != nullptr);
    auto* model = qobject_cast<ProjectSessionTreeModel*>(tree->model());
    QVERIFY(model != nullptr);

    // Pre-check: both projects restored as expanded on startup.
    auto expandedCount = [&]() {
        int n = 0;
        for (int r = 0; r < model->rowCount(QModelIndex()); ++r) {
            const QModelIndex idx = model->index(r, 0);
            if (tree->isExpanded(idx)) ++n;
        }
        return n;
    };
    QCOMPARE(expandedCount(), 2);

    // Flip sort order via settings_kv → triggers model reset.
    // After the reset, expansion must still be there.
    db.setSetting(QStringLiteral("tree.project_sort_order"),
                  QByteArray("alpha"));

    QCOMPARE(expandedCount(), 2);

    // Flip back for good measure.
    db.setSetting(QStringLiteral("tree.project_sort_order"),
                  QByteArray("last_used"));
    QCOMPARE(expandedCount(), 2);
}

// --- indexForSession round-trip ----------------------------------

void TestTreeModel::indexForSessionRoundTrip() {
    auto& db = Persistence::instance();
    const qint64 p = db.createProject(std::nullopt,
                                      QStringLiteral("p"),
                                      QStringLiteral("/p"));
    const qint64 s1 = db.createSession(p, QStringLiteral("s1"),
                                       QStringLiteral("/p"));
    const qint64 s2 = db.createSession(std::nullopt,
                                       QStringLiteral("s2"),
                                       QStringLiteral("/tmp"));

    ProjectSessionTreeModel m;
    const QModelIndex i1 = m.indexForSession(s1);
    const QModelIndex i2 = m.indexForSession(s2);
    QVERIFY(i1.isValid());
    QVERIFY(i2.isValid());
    QCOMPARE(i1.data(ProjectSessionTreeModel::ItemIdRole).toLongLong(), s1);
    QCOMPARE(i2.data(ProjectSessionTreeModel::ItemIdRole).toLongLong(), s2);

    // A non-existent id yields an invalid index.
    QVERIFY(!m.indexForSession(999999).isValid());
}

// --- Drag-drop guards --------------------------------------------

void TestTreeModel::canDropSessionOntoProjectOrRoot() {
    auto& db = Persistence::instance();
    const qint64 p = db.createProject(std::nullopt,
                                      QStringLiteral("dest"),
                                      QStringLiteral("/d"));
    const qint64 s = db.createSession(std::nullopt,
                                      QStringLiteral("mover"),
                                      QStringLiteral("/s"));

    ProjectSessionTreeModel m;
    QMimeData md;
    md.setData(ProjectSessionTreeModel::mimeSessionId(),
               QByteArray::number(static_cast<qlonglong>(s)));

    // Drop onto project → allowed.
    const QModelIndex projIdx = m.index(0, 0);
    QVERIFY(projIdx.isValid());
    QCOMPARE(projIdx.data(ProjectSessionTreeModel::ItemIdRole)
                 .toLongLong(),
             p);
    QVERIFY(m.canDropMimeData(&md, Qt::MoveAction, -1, -1, projIdx));

    // Drop onto invisible root → allowed (orphan).
    QVERIFY(m.canDropMimeData(&md, Qt::MoveAction, -1, -1, QModelIndex()));
}

void TestTreeModel::rejectsProjectDropOnDescendant() {
    auto& db = Persistence::instance();
    const qint64 root = db.createProject(std::nullopt,
                                         QStringLiteral("root"),
                                         QStringLiteral("/r"));
    const qint64 child = db.createProject(root,
                                          QStringLiteral("child"),
                                          QStringLiteral("/r/c"));
    const qint64 grand = db.createProject(child,
                                          QStringLiteral("grand"),
                                          QStringLiteral("/r/c/g"));

    ProjectSessionTreeModel m;
    QMimeData md;
    md.setData(ProjectSessionTreeModel::mimeProjectId(),
               QByteArray::number(static_cast<qlonglong>(root)));

    // Locate `grand` through the model.
    const QModelIndex rootIdx = m.index(0, 0);
    QVERIFY(rootIdx.isValid());
    const QModelIndex childIdx = m.index(0, 0, rootIdx);
    QVERIFY(childIdx.isValid());
    const QModelIndex grandIdx = m.index(0, 0, childIdx);
    QVERIFY(grandIdx.isValid());
    QCOMPARE(grandIdx.data(ProjectSessionTreeModel::ItemIdRole).toLongLong(),
             grand);

    // Dropping `root` onto its descendant must be rejected (cycle).
    QVERIFY(!m.canDropMimeData(&md, Qt::MoveAction, -1, -1, grandIdx));
    QVERIFY(!m.canDropMimeData(&md, Qt::MoveAction, -1, -1, childIdx));
    QVERIFY(!m.canDropMimeData(&md, Qt::MoveAction, -1, -1, rootIdx));

    // But dropping onto the invisible root (→ top-level) is fine.
    QVERIFY(m.canDropMimeData(&md, Qt::MoveAction, -1, -1, QModelIndex()));
}

// --- activateSession bubble-up -----------------------------------

void TestTreeModel::activateSessionBubblesLastUsedUp() {
    auto& db = Persistence::instance();
    const qint64 oldP = db.createProject(std::nullopt,
                                         QStringLiteral("old-root"),
                                         QStringLiteral("/o"));
    QTest::qWait(10);
    const qint64 newP = db.createProject(std::nullopt,
                                         QStringLiteral("new-root"),
                                         QStringLiteral("/n"));
    QTest::qWait(10);
    const qint64 child = db.createProject(oldP,
                                          QStringLiteral("child"),
                                          QStringLiteral("/o/c"));
    QTest::qWait(10);
    const qint64 s = db.createSession(child, QStringLiteral("deep"),
                                      QStringLiteral("/o/c"));

    // Right after creation, newP wins the last_used DESC order.
    {
        ProjectSessionTreeModel m;
        QCOMPARE(m.index(0, 0).data(ProjectSessionTreeModel::ItemIdRole)
                     .toLongLong(),
                 newP);
    }

    QTest::qWait(1100);  // ensure timestamps differ by >=1 second
    db.activateSession(s);

    // Activating the deeply-nested session should float oldP (its
    // ancestor) to the top of the root sibling list.
    ProjectSessionTreeModel m2;
    QCOMPARE(m2.index(0, 0).data(ProjectSessionTreeModel::ItemIdRole)
                 .toLongLong(),
             oldP);
    // child should also be at the top of its sibling list (it has
    // no siblings here, but we check the id anyway).
    const QModelIndex childIdx = m2.index(0, 0, m2.index(0, 0));
    QVERIFY(childIdx.isValid());
    QCOMPARE(childIdx.data(ProjectSessionTreeModel::ItemIdRole).toLongLong(),
             child);
}

// --- MainWindow last_active_id restore ---------------------------

void TestTreeModel::mainWindowRestoresLastActiveSession() {
    auto& db = Persistence::instance();
    const qint64 s1 = db.createSession(
        std::nullopt, QStringLiteral("one"), QStringLiteral("/1"));
    const qint64 s2 = db.createSession(
        std::nullopt, QStringLiteral("two"), QStringLiteral("/2"));

    db.setSetting(QStringLiteral("session.last_active_id"),
                  QByteArray::number(static_cast<qlonglong>(s2)));

    MainWindow w;
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));

    // The startup path should pick s2 over s1 because of the
    // persisted key, even though s1 is older and listSessions
    // may return either order.
    QCOMPARE(w.currentSessionId(), s2);

    // And the status label / chat widgets should reflect it.
    QCOMPARE(w.messageWindow()->currentSessionId(), s2);
    QCOMPARE(w.chatInputWidget()->currentSessionId(), s2);

    Q_UNUSED(s1);
}

// --- MainWindow expanded_ids round-trip --------------------------

void TestTreeModel::mainWindowPersistsExpandedIds() {
    auto& db = Persistence::instance();
    const qint64 p1 = db.createProject(std::nullopt,
                                       QStringLiteral("P1"),
                                       QStringLiteral("/p1"));
    const qint64 p2 = db.createProject(std::nullopt,
                                       QStringLiteral("P2"),
                                       QStringLiteral("/p2"));
    db.createSession(p1, QStringLiteral("s"), QStringLiteral("/p1"));

    // Pre-seed the expanded_ids key so the restore path has
    // something to apply on MainWindow startup.
    const QString csv =
        QString::number(static_cast<qlonglong>(p1)) + QChar(',') +
        QString::number(static_cast<qlonglong>(p2));
    db.setSetting(QStringLiteral("tree.expanded_ids"), csv.toUtf8());

    MainWindow w;
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));

    // Both projects should be expanded post-restore.
    auto* tree = w.findChild<QTreeView*>(QStringLiteral("projectSessionTree"));
    QVERIFY(tree != nullptr);
    auto* model = qobject_cast<ProjectSessionTreeModel*>(tree->model());
    QVERIFY(model != nullptr);

    // Walk both top-level projects and verify isExpanded.
    for (int r = 0; r < model->rowCount(QModelIndex()); ++r) {
        const QModelIndex idx = model->index(r, 0);
        const int kind =
            idx.data(ProjectSessionTreeModel::ItemTypeRole).toInt();
        if (kind != ProjectSessionTreeModel::Project) continue;
        QVERIFY2(tree->isExpanded(idx),
                 qPrintable(QStringLiteral("project row %1 not expanded")
                                .arg(r)));
    }
}

QTEST_MAIN(TestTreeModel)
#include "test_treemodel.moc"
