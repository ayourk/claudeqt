// SPDX-License-Identifier: GPL-3.0-only
//
// M3 multi-process integration test for InstanceHub. Proves that
// events published by the leader reach a client process, and events
// published by a client reach the leader. Uses hub_child_helper as
// the child process, spawned via QProcess.
//
// Timing jitter is injected between steps to exercise race windows
// in the election and relay paths.

#include "instancehub.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>
#include <QtTest/QtTest>

class TestInstanceHubIpc : public QObject {
    Q_OBJECT

private slots:
    void leaderPublishReachesClient();
    void clientPublishReachesLeader();

private:
    QString helperPath() const;
    QStringList readSentinel(const QString& path) const;
};

QString TestInstanceHubIpc::helperPath() const {
    // The helper binary is built alongside the tests.
    return QCoreApplication::applicationDirPath() +
           QStringLiteral("/hub_child_helper");
}

QStringList TestInstanceHubIpc::readSentinel(const QString& path) const {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QStringList lines;
    QTextStream in(&f);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (!line.isEmpty()) lines.append(line);
    }
    return lines;
}

void TestInstanceHubIpc::leaderPublishReachesClient() {
    QVERIFY(QFile::exists(helperPath()));

    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString sockPath =
        QDir(tmp.path()).filePath(QStringLiteral("hub.sock"));
    const QString sentinelPath =
        QDir(tmp.path()).filePath(QStringLiteral("sentinel.txt"));

    InstanceHub::setRelayJitterForTesting(50, 250);
    InstanceHub::setSocketPathForTesting(sockPath);
    auto& hub = InstanceHub::instance();
    hub.start();
    QVERIFY(hub.isLeader());

    // Jittered delay before spawning child — exercises the
    // "leader already bound, child connects" path at varying
    // intervals.
    const int preSpawnJitter =
        QRandomGenerator::global()->bounded(50, 300);
    QTest::qWait(preSpawnJitter);

    QProcess child;
    child.start(helperPath(), {sockPath, sentinelPath});
    QVERIFY(child.waitForStarted(3000));

    // Wait for the child to connect and process the welcome
    // kResync. The child's relay jitter defers emit by up to
    // 250ms, so the total budget must cover: election + connect
    // + jittered emit + sentinel write.
    const int postConnectJitter =
        QRandomGenerator::global()->bounded(500, 1000);
    QTest::qWait(postConnectJitter);

    // Publish an event from the leader.
    InstanceHub::Event ev;
    ev.tag = InstanceHub::kProjectsChanged;
    hub.publish(ev);

    // Give the child time to receive via socket, fire the
    // jittered emit, and write to the sentinel file.
    const int receiveJitter =
        QRandomGenerator::global()->bounded(500, 1000);
    QTest::qWait(receiveJitter);

    // Terminate child gracefully.
    child.terminate();
    child.waitForFinished(3000);

    // Read sentinel — should contain at least: kLeaderAnnounce
    // (from connect welcome), kResync, and kProjectsChanged.
    const QStringList lines = readSentinel(sentinelPath);
    QVERIFY2(lines.size() >= 3,
             qPrintable(QStringLiteral("expected >=3 events, got %1")
                            .arg(lines.size())));

    // Welcome sequence: kLeaderAnnounce (0x6), kResync (0x7f).
    QVERIFY(lines.at(0).startsWith(QStringLiteral("6 ")));
    QVERIFY(lines.at(1).startsWith(QStringLiteral("7f ")));
    // Published event: kProjectsChanged (0x1).
    QVERIFY(lines.at(2).startsWith(QStringLiteral("1 ")));

    hub.shutdown();
    InstanceHub::setSocketPathForTesting(QString());
    InstanceHub::setRelayJitterForTesting(0, 0);
}

void TestInstanceHubIpc::clientPublishReachesLeader() {
    QVERIFY(QFile::exists(helperPath()));

    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString sockPath =
        QDir(tmp.path()).filePath(QStringLiteral("hub.sock"));
    const QString sentinelPath =
        QDir(tmp.path()).filePath(QStringLiteral("sentinel.txt"));

    InstanceHub::setRelayJitterForTesting(50, 250);
    InstanceHub::setSocketPathForTesting(sockPath);
    auto& hub = InstanceHub::instance();
    hub.start();
    QVERIFY(hub.isLeader());

    QSignalSpy eventSpy(&hub, &InstanceHub::eventReceived);

    const int preSpawnJitter =
        QRandomGenerator::global()->bounded(50, 300);
    QTest::qWait(preSpawnJitter);

    // Spawn child with "publish" flag — it will publish a
    // kSessionsChanged after receiving the first inbound event
    // (the kResync welcome).
    QProcess child;
    child.start(helperPath(), {sockPath, sentinelPath,
                               QStringLiteral("publish")});
    QVERIFY(child.waitForStarted(3000));

    // The child receives kResync (deferred by relay jitter), then
    // publishes kSessionsChanged back. The leader receives it via
    // onPeerReadyRead → broadcastFromLeader → deferred emit. Poll
    // until we see the specific event we care about.
    auto sawSessionsChanged = [&]() {
        for (int i = 0; i < eventSpy.count(); ++i) {
            const auto ev =
                eventSpy.at(i).at(0).value<InstanceHub::Event>();
            if (ev.tag == InstanceHub::kSessionsChanged)
                return true;
        }
        return false;
    };
    QTRY_VERIFY_WITH_TIMEOUT(sawSessionsChanged(), 5000);

    child.terminate();
    child.waitForFinished(3000);

    hub.shutdown();
    InstanceHub::setSocketPathForTesting(QString());
    InstanceHub::setRelayJitterForTesting(0, 0);
}

QTEST_MAIN(TestInstanceHubIpc)
#include "test_instancehub_ipc.moc"
