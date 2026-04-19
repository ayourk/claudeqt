// SPDX-License-Identifier: GPL-3.0-only
//
// Unit tests for the Persistence DAO singleton. Coverage:
// fresh-DB migration from v0, project CRUD roundtrip, session CRUD
// including nullable project_id, cwd retarget, buffer dirty-stash
// roundtrip, messages append + list, settings_kv get/set +
// fallback.
//
// Each test method uses QTemporaryDir to get an ephemeral DB path
// and calls Persistence::setDatabasePathForTesting() *before*
// touching instance(). The singleton is process-wide, so the tests
// must share one DB across initTestCase / cleanupTestCase rather
// than per-test.

#include "persistence.h"
#include "instancehub.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class TestPersistence : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init() { QTest::qWait(300); }
    void freshDbHasV1Schema();
    void projectCrudRoundtrip();
    void projectNestingAndCycleGuard();
    void sessionCrudRoundtrip();
    void sessionOrphanAdoption();
    void sessionCwdRetarget();
    void bufferDirtyStashRoundtrip();
    void messagesAppendAndList();
    void settingsGetSetWithFallback();
    void cleanupTestCase();

private:
    QTemporaryDir m_tmp;
};

void TestPersistence::initTestCase() {
    QVERIFY(m_tmp.isValid());
    const QString dbPath = m_tmp.filePath(QStringLiteral("test.sqlite"));
    Persistence::setDatabasePathForTesting(dbPath);
    Persistence::setDeferralJitterForTesting(50, 250);

    const QString sockPath =
        m_tmp.filePath(QStringLiteral("hub.sock"));
    InstanceHub::setSocketPathForTesting(sockPath);
    InstanceHub::instance().start();

    QCoreApplication::setApplicationName(QStringLiteral("persistence_test"));

    // First touch of instance() opens the DB and runs migrations.
    (void) Persistence::instance();
    Persistence::instance().connectToHub();
    QVERIFY(QFile::exists(dbPath));
}

void TestPersistence::cleanupTestCase() {
    InstanceHub::instance().shutdown();
    InstanceHub::setSocketPathForTesting(QString());
}

void TestPersistence::freshDbHasV1Schema() {
    // A smoke check: creating a project on a fresh DB exercises every
    // migration that ran. If the projects table doesn't exist this
    // throws.
    auto& p = Persistence::instance();
    const qint64 id = p.createProject(std::nullopt,
                                      QStringLiteral("schema-probe"),
                                      QStringLiteral("/tmp/probe"));
    QVERIFY(id > 0);
    p.deleteProject(id);
}

void TestPersistence::projectCrudRoundtrip() {
    auto& p = Persistence::instance();
    QSignalSpy spy(&p, &Persistence::projectsChanged);

    const qint64 id = p.createProject(std::nullopt,
                                      QStringLiteral("alpha"),
                                      QStringLiteral("/srv/alpha"));
    QVERIFY(id > 0);
    QTRY_COMPARE(spy.count(), 1);

    auto projects = p.listTopLevelProjects();
    bool found = false;
    for (const auto& r : projects) {
        if (r.id == id) {
            QCOMPARE(r.name, QStringLiteral("alpha"));
            QCOMPARE(r.rootPath, QStringLiteral("/srv/alpha"));
            QVERIFY(!r.parentProjectId.has_value());
            QVERIFY(r.created > 0);
            QVERIFY(r.updated >= r.created);
            QVERIFY(r.lastUsed >= r.created);
            found = true;
            break;
        }
    }
    QVERIFY(found);

    p.renameProject(id, QStringLiteral("alpha-renamed"));
    projects = p.listTopLevelProjects();
    for (const auto& r : projects) {
        if (r.id == id) {
            QCOMPARE(r.name, QStringLiteral("alpha-renamed"));
        }
    }

    p.deleteProject(id);
    projects = p.listTopLevelProjects();
    for (const auto& r : projects) {
        QVERIFY(r.id != id);
    }
}

void TestPersistence::projectNestingAndCycleGuard() {
    auto& p = Persistence::instance();

    const qint64 root = p.createProject(std::nullopt,
                                        QStringLiteral("nest-root"),
                                        QStringLiteral("/a"));
    const qint64 child = p.createProject(root,
                                         QStringLiteral("nest-child"),
                                         QStringLiteral("/a/b"));
    const qint64 grandchild =
        p.createProject(child, QStringLiteral("nest-grandchild"),
                        QStringLiteral("/a/b/c"));

    auto children = p.listChildProjects(root);
    bool sawChild = false;
    for (const auto& r : children) {
        if (r.id == child) sawChild = true;
    }
    QVERIFY(sawChild);

    // Cycle guard: reparenting root under grandchild should throw.
    bool threw = false;
    try {
        p.reparentProject(root, grandchild);
    } catch (const std::exception&) {
        threw = true;
    }
    QVERIFY2(threw, "reparentProject did not refuse a cycle");

    // Legitimate reparent: move grandchild back under root.
    p.reparentProject(grandchild, root);
    children = p.listChildProjects(root);
    bool sawGrandchild = false;
    for (const auto& r : children) {
        if (r.id == grandchild) sawGrandchild = true;
    }
    QVERIFY(sawGrandchild);

    // Deleting a parent promotes children to top-level (ON DELETE SET NULL).
    p.deleteProject(root);
    const auto topLevel = p.listTopLevelProjects();
    bool childPromoted = false;
    bool grandchildPromoted = false;
    for (const auto& r : topLevel) {
        if (r.id == child) childPromoted = true;
        if (r.id == grandchild) grandchildPromoted = true;
    }
    QVERIFY(childPromoted);
    QVERIFY(grandchildPromoted);

    p.deleteProject(child);
    p.deleteProject(grandchild);
}

void TestPersistence::sessionCrudRoundtrip() {
    auto& p = Persistence::instance();

    const qint64 projectId = p.createProject(std::nullopt,
                                             QStringLiteral("sess-proj"),
                                             QStringLiteral("/s"));
    const qint64 sid = p.createSession(projectId,
                                       QStringLiteral("session-one"),
                                       QStringLiteral("/s/work"));
    QVERIFY(sid > 0);

    auto sessions = p.listSessionsInProject(projectId);
    QCOMPARE(sessions.size(), 1);
    QCOMPARE(sessions.first().id, sid);
    QCOMPARE(sessions.first().title, QStringLiteral("session-one"));
    QCOMPARE(sessions.first().cwd, QStringLiteral("/s/work"));
    QVERIFY(sessions.first().projectId.has_value());
    QCOMPARE(*sessions.first().projectId, projectId);

    p.renameSession(sid, QStringLiteral("session-renamed"));
    sessions = p.listSessionsInProject(projectId);
    QCOMPARE(sessions.first().title, QStringLiteral("session-renamed"));

    p.deleteSession(sid);
    sessions = p.listSessionsInProject(projectId);
    QCOMPARE(sessions.size(), 0);

    p.deleteProject(projectId);
}

void TestPersistence::sessionOrphanAdoption() {
    auto& p = Persistence::instance();

    // Orphan session: nullopt project_id.
    const qint64 sid = p.createSession(std::nullopt,
                                       QStringLiteral("orphan"),
                                       QStringLiteral("/tmp/orphan"));
    QVERIFY(sid > 0);

    auto orphans = p.listOrphanSessions();
    bool foundOrphan = false;
    for (const auto& s : orphans) {
        if (s.id == sid) {
            QVERIFY(!s.projectId.has_value());
            foundOrphan = true;
        }
    }
    QVERIFY(foundOrphan);

    // Adopt into a project.
    const qint64 projectId = p.createProject(std::nullopt,
                                             QStringLiteral("adopter"),
                                             QStringLiteral("/a"));
    p.moveSessionToProject(sid, projectId);

    auto inProject = p.listSessionsInProject(projectId);
    bool adopted = false;
    for (const auto& s : inProject) {
        if (s.id == sid) adopted = true;
    }
    QVERIFY(adopted);

    // Demote back to orphan.
    p.moveSessionToProject(sid, std::nullopt);
    orphans = p.listOrphanSessions();
    bool reOrphan = false;
    for (const auto& s : orphans) {
        if (s.id == sid) reOrphan = true;
    }
    QVERIFY(reOrphan);

    p.deleteSession(sid);
    p.deleteProject(projectId);
}

void TestPersistence::sessionCwdRetarget() {
    auto& p = Persistence::instance();

    const qint64 sid = p.createSession(std::nullopt,
                                       QStringLiteral("retarget-me"),
                                       QStringLiteral("/old/path"));
    p.retargetSessionCwd(sid, QStringLiteral("/new/path"));

    const auto all = p.listOrphanSessions();
    bool checked = false;
    for (const auto& s : all) {
        if (s.id == sid) {
            QCOMPARE(s.cwd, QStringLiteral("/new/path"));
            checked = true;
        }
    }
    QVERIFY(checked);
    p.deleteSession(sid);
}

void TestPersistence::bufferDirtyStashRoundtrip() {
    auto& p = Persistence::instance();

    const qint64 sid = p.createSession(std::nullopt,
                                       QStringLiteral("buf-sess"),
                                       QStringLiteral("/b"));
    const qint64 bid = p.openBuffer(sid, QStringLiteral("/b/file.cpp"));
    QVERIFY(bid > 0);

    const QByteArray dirty = "unsaved edits on line 42\n";
    p.stashDirtyBuffer(bid, dirty);

    auto loaded = p.loadBuffer(bid);
    QVERIFY(loaded.has_value());
    QCOMPARE(loaded->dirtyContent, dirty);
    QVERIFY(!loaded->savedMtime.has_value());

    // Simulate a save: clear dirty_content, stamp mtime.
    p.saveBufferContent(bid, QByteArray(), 1234567890);
    loaded = p.loadBuffer(bid);
    QVERIFY(loaded.has_value());
    QVERIFY(loaded->dirtyContent.isEmpty());
    QVERIFY(loaded->savedMtime.has_value());
    QCOMPARE(*loaded->savedMtime, qint64(1234567890));

    p.closeBuffer(bid);
    loaded = p.loadBuffer(bid);
    QVERIFY(!loaded.has_value());

    p.deleteSession(sid);
}

void TestPersistence::messagesAppendAndList() {
    auto& p = Persistence::instance();

    const qint64 sid = p.createSession(std::nullopt,
                                       QStringLiteral("msg-sess"),
                                       QStringLiteral("/m"));

    QSignalSpy spy(&p, &Persistence::messagesAppended);
    p.appendMessage(sid, QStringLiteral("user"), QByteArray("hello"));
    p.appendMessage(sid, QStringLiteral("user"), QByteArray("again"));
    QCOMPARE(spy.count(), 2);

    const auto msgs = p.listMessagesForSession(sid);
    QCOMPARE(msgs.size(), 2);
    QCOMPARE(msgs.at(0).content, QByteArray("hello"));
    QCOMPARE(msgs.at(0).msgRole, QStringLiteral("user"));
    QCOMPARE(msgs.at(1).content, QByteArray("again"));

    // Cascade: deleting the session should remove its messages.
    p.deleteSession(sid);
    const auto after = p.listMessagesForSession(sid);
    QCOMPARE(after.size(), 0);
}

void TestPersistence::settingsGetSetWithFallback() {
    auto& p = Persistence::instance();

    // Unset key returns fallback verbatim.
    const QByteArray fallback = "default-value";
    QCOMPARE(p.getSetting(QStringLiteral("not.set"), fallback), fallback);

    // Set then read returns the stored value.
    p.setSetting(QStringLiteral("ui.theme"), QByteArray("dark"));
    QCOMPARE(p.getSetting(QStringLiteral("ui.theme"), fallback),
             QByteArray("dark"));

    // Upsert: write a new value for the same key.
    p.setSetting(QStringLiteral("ui.theme"), QByteArray("light"));
    QCOMPARE(p.getSetting(QStringLiteral("ui.theme"), fallback),
             QByteArray("light"));

    // Binary-safe: values can contain null bytes.
    const QByteArray binary("\x00\xff\x01\x02", 4);
    p.setSetting(QStringLiteral("binary.blob"), binary);
    QCOMPARE(p.getSetting(QStringLiteral("binary.blob"), fallback), binary);
}

QTEST_MAIN(TestPersistence)
#include "test_persistence.moc"
