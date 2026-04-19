// SPDX-License-Identifier: GPL-3.0-only
//
// M1 coverage for the multi-instance hub:
//   - Wire codec round-trip across every event tag.
//   - Partial-frame handling: split payload, reassembled reads.
//   - Malformed length rejection (negative, oversized).
//   - Single-process leader election via setSocketPathForTesting
//     (bind succeeds → isLeader()==true; kResync is emitted).
//
// Cross-process election and fan-out are deferred until
// Persistence is wired to the hub and there's something meaningful
// for a second process to observe.

#include "instancehub.h"

#include <QCoreApplication>
#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QtTest/QtTest>

class TestInstanceHub : public QObject {
    Q_OBJECT

private slots:
    void codecRoundTripEveryTag();
    void codecHandlesPartialFrames();
    void codecRejectsNegativeLength();
    void codecRejectsOversizedFrame();
    void startBindsAsLeaderOnFreshPath();
    void epochIncreasesOnReElection();
    void publishedEventsCarryEpoch();
};

// --- Codec --------------------------------------------------------

void TestInstanceHub::codecRoundTripEveryTag() {
    // QCOMPARE inside a lambda expands to `return;`, which conflicts
    // with the lambda's trailing `return out.first();` (the compiler
    // deduces void). Use QVERIFY here — its failure path calls
    // QTest::qVerify without a bare return in this scope.
    auto roundtrip = [](const InstanceHub::Event& in) {
        const QByteArray frame = InstanceHub::encodeFrame(in);
        QByteArray buf = frame;
        QList<InstanceHub::Event> out;
        InstanceHub::decodeFrames(buf, out);
        QTest::qVerify(buf.size() == 0, "buf drained", "",
                       __FILE__, __LINE__);
        QTest::qVerify(out.size() == 1, "one event out", "",
                       __FILE__, __LINE__);
        return out.first();
    };

    InstanceHub::Event a;
    a.tag = InstanceHub::kProjectsChanged;
    a.seqno = 42;
    a.epoch = 1713200000000LL;
    auto ra = roundtrip(a);
    QCOMPARE(ra.tag, InstanceHub::kProjectsChanged);
    QCOMPARE(ra.seqno, qint64(42));
    QCOMPARE(ra.epoch, qint64(1713200000000LL));

    InstanceHub::Event b;
    b.tag = InstanceHub::kSessionsChanged;
    b.seqno = 43;
    QCOMPARE(roundtrip(b).tag, InstanceHub::kSessionsChanged);

    InstanceHub::Event c;
    c.tag = InstanceHub::kMessagesAppended;
    c.seqno = 100;
    c.sessionId = 7;
    c.messageId = 19;
    auto rc = roundtrip(c);
    QCOMPARE(rc.tag, InstanceHub::kMessagesAppended);
    QCOMPARE(rc.sessionId, qint64(7));
    QCOMPARE(rc.messageId, qint64(19));

    InstanceHub::Event d;
    d.tag = InstanceHub::kBuffersChanged;
    d.seqno = 101;
    d.sessionId = 9;
    QCOMPARE(roundtrip(d).sessionId, qint64(9));

    InstanceHub::Event e;
    e.tag = InstanceHub::kSettingChanged;
    e.seqno = 102;
    e.settingName = QStringLiteral("editor.font.size");
    QCOMPARE(roundtrip(e).settingName,
             QStringLiteral("editor.font.size"));

    InstanceHub::Event la;
    la.tag = InstanceHub::kLeaderAnnounce;
    la.seqno = 50;
    la.epoch = 1713200000001LL;
    auto rla = roundtrip(la);
    QCOMPARE(rla.tag, InstanceHub::kLeaderAnnounce);
    QCOMPARE(rla.epoch, qint64(1713200000001LL));

    InstanceHub::Event r;
    r.tag = InstanceHub::kResync;
    r.seqno = 1;
    QCOMPARE(roundtrip(r).tag, InstanceHub::kResync);
}

void TestInstanceHub::codecHandlesPartialFrames() {
    // Two concatenated frames, fed one byte at a time. Decoder
    // must emit exactly when each full frame is buffered and
    // leave the trailing partial bytes alone between feeds.
    InstanceHub::Event e1;
    e1.tag = InstanceHub::kMessagesAppended;
    e1.seqno = 1;
    e1.sessionId = 10;
    e1.messageId = 20;
    InstanceHub::Event e2;
    e2.tag = InstanceHub::kSettingChanged;
    e2.seqno = 2;
    e2.settingName = QStringLiteral("ui.theme");

    const QByteArray f1 = InstanceHub::encodeFrame(e1);
    const QByteArray f2 = InstanceHub::encodeFrame(e2);
    const QByteArray combined = f1 + f2;

    QByteArray buf;
    QList<InstanceHub::Event> out;

    for (int i = 0; i < combined.size(); ++i) {
        buf.append(combined.at(i));
        InstanceHub::decodeFrames(buf, out);
        if (i + 1 < f1.size()) {
            QCOMPARE(out.size(), 0);
        } else if (i + 1 < combined.size()) {
            QCOMPARE(out.size(), 1);
        }
    }

    QCOMPARE(out.size(), 2);
    QCOMPARE(out[0].tag, InstanceHub::kMessagesAppended);
    QCOMPARE(out[0].messageId, qint64(20));
    QCOMPARE(out[1].tag, InstanceHub::kSettingChanged);
    QCOMPARE(out[1].settingName, QStringLiteral("ui.theme"));
    QCOMPARE(buf.size(), 0);
}

void TestInstanceHub::codecRejectsNegativeLength() {
    QByteArray buf;
    QDataStream ds(&buf, QIODevice::WriteOnly);
    ds.setVersion(QDataStream::Qt_6_0);
    ds << qint32(-1);
    buf.append(QByteArray(8, 0));  // body filler

    QList<InstanceHub::Event> out;
    InstanceHub::decodeFrames(buf, out);
    // Malformed → decoder drops the buffer, emits nothing.
    QCOMPARE(out.size(), 0);
    QCOMPARE(buf.size(), 0);
}

void TestInstanceHub::codecRejectsOversizedFrame() {
    QByteArray buf;
    QDataStream ds(&buf, QIODevice::WriteOnly);
    ds.setVersion(QDataStream::Qt_6_0);
    ds << qint32(8 * 1024 * 1024);  // > 1 MiB cap

    QList<InstanceHub::Event> out;
    InstanceHub::decodeFrames(buf, out);
    QCOMPARE(out.size(), 0);
    QCOMPARE(buf.size(), 0);
}

// --- Election -----------------------------------------------------

void TestInstanceHub::startBindsAsLeaderOnFreshPath() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString sockPath =
        QDir(tmp.path()).filePath(QStringLiteral("hub.sock"));
    InstanceHub::setSocketPathForTesting(sockPath);

    auto& hub = InstanceHub::instance();
    QSignalSpy leaderSpy(&hub, &InstanceHub::leaderChanged);
    QSignalSpy eventSpy(&hub, &InstanceHub::eventReceived);

    hub.start();

    QVERIFY(hub.isLeader());
    QCOMPARE(leaderSpy.count(), 1);
    QCOMPARE(leaderSpy.at(0).at(0).toBool(), true);

    // becomeLeader() broadcasts kLeaderAnnounce (seqno=1) then
    // kResync (seqno=2).
    QCOMPARE(eventSpy.count(), 2);
    const auto ev0 = eventSpy.at(0).at(0).value<InstanceHub::Event>();
    QCOMPARE(ev0.tag, InstanceHub::kLeaderAnnounce);
    QCOMPARE(ev0.seqno, qint64(1));
    QVERIFY(ev0.epoch > 0);
    const auto ev1 = eventSpy.at(1).at(0).value<InstanceHub::Event>();
    QCOMPARE(ev1.tag, InstanceHub::kResync);
    QCOMPARE(ev1.seqno, qint64(2));
    QCOMPARE(ev1.epoch, ev0.epoch);

    // Publishing on the leader path stamps the next seqno.
    InstanceHub::Event out;
    out.tag = InstanceHub::kSessionsChanged;
    hub.publish(out);
    QCOMPARE(eventSpy.count(), 3);
    QCOMPARE(hub.lastSeqno(), qint64(3));
    QCOMPARE(hub.epoch(), ev0.epoch);

    hub.shutdown();
    InstanceHub::setSocketPathForTesting(QString());
}

void TestInstanceHub::epochIncreasesOnReElection() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString sockPath =
        QDir(tmp.path()).filePath(QStringLiteral("hub.sock"));
    InstanceHub::setSocketPathForTesting(sockPath);

    auto& hub = InstanceHub::instance();
    hub.start();
    QVERIFY(hub.isLeader());
    const qint64 epoch1 = hub.epoch();
    QVERIFY(epoch1 > 0);

    hub.shutdown();

    // Small wait to guarantee the msec timestamp advances.
    QTest::qWait(5);

    hub.start();
    QVERIFY(hub.isLeader());
    const qint64 epoch2 = hub.epoch();
    QVERIFY2(epoch2 > epoch1,
             qPrintable(QStringLiteral("epoch2 %1 should exceed epoch1 %2")
                            .arg(epoch2).arg(epoch1)));

    // Seqno resets on new leadership term.
    QCOMPARE(hub.lastSeqno(), qint64(2));

    hub.shutdown();
    InstanceHub::setSocketPathForTesting(QString());
}

void TestInstanceHub::publishedEventsCarryEpoch() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString sockPath =
        QDir(tmp.path()).filePath(QStringLiteral("hub.sock"));
    InstanceHub::setSocketPathForTesting(sockPath);

    auto& hub = InstanceHub::instance();
    QSignalSpy eventSpy(&hub, &InstanceHub::eventReceived);

    hub.start();
    QVERIFY(hub.isLeader());

    InstanceHub::Event ev;
    ev.tag = InstanceHub::kProjectsChanged;
    hub.publish(ev);

    // Spy sees: kLeaderAnnounce, kResync (from becomeLeader),
    // plus our kProjectsChanged.
    QCOMPARE(eventSpy.count(), 3);
    for (int i = 0; i < eventSpy.count(); ++i) {
        const auto e =
            eventSpy.at(i).at(0).value<InstanceHub::Event>();
        QCOMPARE(e.epoch, hub.epoch());
    }

    hub.shutdown();
    InstanceHub::setSocketPathForTesting(QString());
}

QTEST_MAIN(TestInstanceHub)
#include "test_instancehub.moc"
