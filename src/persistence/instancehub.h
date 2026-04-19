// SPDX-License-Identifier: GPL-3.0-only
//
// Local multi-instance change hub per docs/multi-instance-hub.md.
// QLocalServer-based pub/sub: first claudeqt process to bind is
// the hub; subsequent instances connect as clients. Every DAO
// write publishes an Event; every peer receives it through
// eventReceived() and re-drives its local model signals. See the
// design doc for wire format, election, and failover details.

#pragma once

#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QPointer>
#include <QString>

class QLocalServer;
class QLocalSocket;
class QTimer;

class InstanceHub : public QObject {
    Q_OBJECT
public:
    // One-byte event tags. Stable wire identifiers; never reuse a
    // retired tag — bump to the next unused value.
    enum EventTag : quint8 {
        kProjectsChanged = 0x01,
        kSessionsChanged = 0x02,
        kMessagesAppended = 0x03,
        kBuffersChanged = 0x04,
        kSettingChanged = 0x05,
        kLeaderAnnounce = 0x06,
        kDraftChanged = 0x07,
        kResync = 0x7F,
        // 0x80+ reserved for future streaming deltas / tool calls.
    };

    struct Event {
        EventTag tag = kProjectsChanged;
        // Stamped by the hub on broadcast. 0 means "not yet
        // published" (client→hub publish sets seqno to 0; the hub
        // overwrites before fan-out).
        qint64 seqno = 0;
        // Leader epoch — millisecond timestamp assigned when a
        // process becomes leader. Clients latch the highest epoch
        // they've seen and ignore events from lower ones, preventing
        // stale-leader confusion after failover.
        qint64 epoch = 0;
        // Optional payload depending on tag. For kMessagesAppended:
        // sessionId, messageId. For kBuffersChanged: sessionId.
        // For kSettingChanged: the setting name in settingName.
        qint64 sessionId = 0;
        qint64 messageId = 0;
        QString settingName;
    };

    static InstanceHub& instance();

    // Idempotent. Runs the bind-race election. Safe to call more
    // than once; second call is a no-op.
    void start();

    // Shut down sockets cleanly — called from main() on app exit
    // so the hub doesn't leave its socket path stale for the next
    // instance to trip over.
    void shutdown();

    bool isLeader() const { return m_isLeader; }
    qint64 lastSeqno() const { return m_lastSeqno; }
    qint64 epoch() const { return m_epoch; }

    // Non-blocking. Buffers and flushes on the event loop so
    // callers inside a QSqlQuery::exec() don't pay socket latency.
    void publish(const Event& e);

    // Override the default socket path. Tests use this so two
    // InstanceHub objects in the same process (or under pytest-xdist)
    // don't collide with the real user hub.
    static void setSocketPathForTesting(const QString& absolutePath);

    // Inflate event delivery delays to expose race conditions in
    // tests. Affects broadcastFromLeader (leader-side local emit)
    // and onClientReadyRead (client-side local emit). Each emit
    // fires after a random delay in [minMs, maxMs] instead of
    // synchronously. Production default is 0 (synchronous).
    static void setRelayJitterForTesting(int minMs, int maxMs);

    // Override election cooldown range. Production default scales
    // with m_electionAttempts; tests use fixed ranges for
    // determinism.
    static void setElectionJitterForTesting(int minMs, int maxMs);

    // Wire codec exposed for tests. `encodeFrame` produces a
    // ready-to-send QByteArray (length prefix + body). `decodeFrames`
    // parses as many complete frames as possible out of `buf`,
    // appending to `out` and consuming the matching prefix of `buf`.
    // Any trailing partial frame is left in `buf` untouched.
    static QByteArray encodeFrame(const Event& e);
    static void decodeFrames(QByteArray& buf, QList<Event>& out);

signals:
    // Fires on every instance after an event propagates. The
    // leader re-emits its own publishes through this signal too,
    // so downstream consumers don't need two code paths.
    void eventReceived(const InstanceHub::Event& e);

    // Diagnostic: fired when this process becomes (or stops
    // being) the hub. Hooked by the status bar.
    void leaderChanged(bool isLeader);

private:
    InstanceHub();
    ~InstanceHub() override;
    InstanceHub(const InstanceHub&) = delete;
    InstanceHub& operator=(const InstanceHub&) = delete;

    QString computeSocketPath() const;

    // Election: try to become the hub. On success, spin up the
    // server and start accepting clients. On failure, connect as
    // a client.
    void runElection();
    void becomeLeader();
    void becomeClient();

    // Leader-side wiring for newly accepted clients.
    void onNewConnection();
    // Client-side socket handlers.
    void onClientReadyRead();
    void onClientDisconnected();
    // Leader-side read handler for connected peers.
    void onPeerReadyRead();
    void onPeerDisconnected();

    // Hub-only: fan a freshly-stamped event out to every peer
    // socket, then emit eventReceived locally.
    void broadcastFromLeader(Event e);

    int electionCooldownMs() const;

    QString m_socketPath;
    bool m_started = false;
    bool m_isLeader = false;
    qint64 m_lastSeqno = 0;
    qint64 m_epoch = 0;
    int m_electionAttempts = 0;

    // Leader state.
    QPointer<QLocalServer> m_server;
    struct Peer {
        QLocalSocket* socket = nullptr;
        QByteArray readBuf;
    };
    QList<Peer> m_peers;

    // Client state.
    QPointer<QLocalSocket> m_clientSocket;
    QByteArray m_clientReadBuf;

    // Backoff timer for re-election after the leader we were
    // connected to goes away.
    QTimer* m_reelectTimer = nullptr;
};

Q_DECLARE_METATYPE(InstanceHub::Event)
