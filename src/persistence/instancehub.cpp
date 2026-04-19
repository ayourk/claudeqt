// SPDX-License-Identifier: GPL-3.0-only

#include "instancehub.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QLocalServer>
#include <QLocalSocket>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTimer>

namespace {

// Base election cooldown range in milliseconds. With exponential
// backoff the actual range is [base_min * 2^attempt, base_max * 2^attempt],
// capped at kElectionCooldownCapMs. Wide enough to separate 16
// racing instances while short enough that users don't perceive
// the boot delay on the first attempt.
constexpr int kElectionCooldownBaseMinMs = 200;
constexpr int kElectionCooldownBaseMaxMs = 1000;
constexpr int kElectionCooldownCapMs = 5000;

// One frame on the wire is a qint32 length prefix followed by
// `length` bytes of body. We refuse frames larger than 1 MiB
// defensively — every current event is tiny, and a rogue length
// header shouldn't be able to force the reader to allocate
// gigabytes.
constexpr qint32 kMaxFrameBodyBytes = 1 * 1024 * 1024;

QString* testingSocketPathOverride() {
    static QString s;
    return &s;
}

struct RelayJitter {
    int minMs = 0;
    int maxMs = 0;
};

RelayJitter& relayJitterRef() {
    static RelayJitter j;
    return j;
}

int relayDelayMs() {
    const auto& j = relayJitterRef();
    if (j.maxMs <= 0) return 0;
    return QRandomGenerator::global()->bounded(j.minMs, j.maxMs + 1);
}

struct ElectionJitter {
    int minMs = 0;
    int maxMs = 0;
};

ElectionJitter& electionJitterRef() {
    static ElectionJitter j;
    return j;
}

}  // namespace

// ---- Singleton ----------------------------------------------------

InstanceHub& InstanceHub::instance() {
    static InstanceHub s;
    return s;
}

void InstanceHub::setSocketPathForTesting(const QString& absolutePath) {
    *testingSocketPathOverride() = absolutePath;
}

void InstanceHub::setRelayJitterForTesting(int minMs, int maxMs) {
    auto& j = relayJitterRef();
    j.minMs = minMs;
    j.maxMs = maxMs;
}

void InstanceHub::setElectionJitterForTesting(int minMs, int maxMs) {
    auto& j = electionJitterRef();
    j.minMs = minMs;
    j.maxMs = maxMs;
}

InstanceHub::InstanceHub() {
    qRegisterMetaType<InstanceHub::Event>();
    m_reelectTimer = new QTimer(this);
    m_reelectTimer->setSingleShot(true);
    connect(m_reelectTimer, &QTimer::timeout, this,
            &InstanceHub::runElection);
}

InstanceHub::~InstanceHub() = default;

// ---- Lifecycle ----------------------------------------------------

void InstanceHub::start() {
    if (m_started) return;
    m_started = true;
    m_socketPath = computeSocketPath();
    runElection();
}

void InstanceHub::shutdown() {
    if (m_server) {
        // QLocalServer::close() doesn't unlink the socket path on
        // POSIX; leaving the file there would confuse the next
        // launch's listen() into thinking someone else is bound.
        m_server->close();
        QLocalServer::removeServer(m_socketPath);
        m_server->deleteLater();
        m_server = nullptr;
    }
    for (auto& p : m_peers) {
        if (p.socket) {
            p.socket->disconnectFromServer();
            p.socket->deleteLater();
        }
    }
    m_peers.clear();
    if (m_clientSocket) {
        m_clientSocket->disconnectFromServer();
        m_clientSocket->deleteLater();
        m_clientSocket = nullptr;
    }
    m_started = false;
    if (m_isLeader) {
        m_isLeader = false;
        emit leaderChanged(false);
    }
}

QString InstanceHub::computeSocketPath() const {
    if (!testingSocketPathOverride()->isEmpty()) {
        return *testingSocketPathOverride();
    }
    // RuntimeLocation is per-user on Linux (/run/user/$UID) and
    // gracefully falls back to TempLocation where it isn't
    // defined. That's the same directory systemd uses for its
    // own user sockets so lifetime semantics match.
    QString dir = QStandardPaths::writableLocation(
        QStandardPaths::RuntimeLocation);
    if (dir.isEmpty()) {
        dir = QStandardPaths::writableLocation(
            QStandardPaths::TempLocation);
    }
    QDir().mkpath(dir);
    return QDir(dir).filePath(QCoreApplication::applicationName() + QStringLiteral("-hub"));
}

int InstanceHub::electionCooldownMs() const {
    const auto& ej = electionJitterRef();
    if (ej.maxMs > 0) {
        return QRandomGenerator::global()->bounded(ej.minMs,
                                                   ej.maxMs + 1);
    }
    const int scale = 1 << qMin(m_electionAttempts, 4);
    const int lo = qMin(kElectionCooldownBaseMinMs * scale,
                        kElectionCooldownCapMs);
    const int hi = qMin(kElectionCooldownBaseMaxMs * scale,
                        kElectionCooldownCapMs);
    return QRandomGenerator::global()->bounded(lo, hi + 1);
}

// ---- Election -----------------------------------------------------

void InstanceHub::runElection() {
    // Drop any stale client socket before we try to become leader.
    if (m_clientSocket) {
        m_clientSocket->disconnectFromServer();
        m_clientSocket->deleteLater();
        m_clientSocket = nullptr;
    }
    m_clientReadBuf.clear();

    auto* server = new QLocalServer(this);
    // First `listen()` attempt: don't call removeServer() — if
    // another live instance already holds the socket we want to
    // fall through to the client branch, not stomp it.
    if (server->listen(m_socketPath)) {
        m_server = server;
        m_electionAttempts = 0;
        becomeLeader();
        return;
    }

    // Listen failed. Two sub-cases:
    //   a) another live process holds the socket → connect as
    //      client.
    //   b) a previous process crashed and left a stale socket
    //      file → no one is listening, connect() will fail, and
    //      we should clean up then retry listen() with the path
    //      freed.
    delete server;

    auto* client = new QLocalSocket(this);
    client->connectToServer(m_socketPath);
    if (client->waitForConnected(100)) {
        m_clientSocket = client;
        m_electionAttempts = 0;
        connect(client, &QLocalSocket::readyRead, this,
                &InstanceHub::onClientReadyRead);
        connect(client, &QLocalSocket::disconnected, this,
                &InstanceHub::onClientDisconnected);
        becomeClient();
        return;
    }
    delete client;

    // Socket path exists but nothing answered — stale file from a
    // crashed peer. Remove and retry listen() directly (no
    // further election round, no risk of a loop because
    // removeServer() unlinks the path unconditionally).
    QLocalServer::removeServer(m_socketPath);
    auto* retry = new QLocalServer(this);
    if (retry->listen(m_socketPath)) {
        m_server = retry;
        m_electionAttempts = 0;
        becomeLeader();
        return;
    }
    // Still failing — something is racing. Schedule a retry with
    // exponential backoff so we don't hot-spin against siblings.
    delete retry;
    ++m_electionAttempts;
    m_reelectTimer->start(electionCooldownMs());
}

void InstanceHub::becomeLeader() {
    connect(m_server.data(), &QLocalServer::newConnection, this,
            &InstanceHub::onNewConnection);
    const bool changed = !m_isLeader;
    m_isLeader = true;
    m_lastSeqno = 0;
    m_epoch = QDateTime::currentMSecsSinceEpoch();
    if (changed) {
        emit leaderChanged(true);
    }
    // Announce leadership so clients latch our epoch, then
    // kResync so they re-query canonical state.
    Event announce;
    announce.tag = kLeaderAnnounce;
    broadcastFromLeader(announce);
    Event resync;
    resync.tag = kResync;
    broadcastFromLeader(resync);
}

void InstanceHub::becomeClient() {
    if (m_isLeader) {
        m_isLeader = false;
        emit leaderChanged(false);
    }
}

// ---- Publish / broadcast ------------------------------------------

void InstanceHub::publish(const Event& e) {
    if (!m_started) return;
    if (m_isLeader) {
        // Hub path: stamp seqno locally and fan out.
        Event stamped = e;
        broadcastFromLeader(stamped);
        return;
    }
    // Client path: send to hub, which will stamp and broadcast.
    if (!m_clientSocket ||
        m_clientSocket->state() != QLocalSocket::ConnectedState) {
        // Not connected right now — in-flight election. Drop.
        // Current callers are all "a write just landed, peers
        // should re-query" semantics; dropping loses timeliness
        // but not data (the next kResync or the next publish
        // once we reconnect will prompt the re-query).
        return;
    }
    Event outgoing = e;
    outgoing.seqno = 0;
    const QByteArray frame = encodeFrame(outgoing);
    m_clientSocket->write(frame);
}

void InstanceHub::broadcastFromLeader(Event e) {
    e.seqno = ++m_lastSeqno;
    e.epoch = m_epoch;
    const QByteArray frame = encodeFrame(e);
    for (auto& p : m_peers) {
        if (p.socket &&
            p.socket->state() == QLocalSocket::ConnectedState) {
            p.socket->write(frame);
        }
    }
    const int delay = relayDelayMs();
    if (delay > 0) {
        QTimer::singleShot(delay, this, [this, e]() {
            emit eventReceived(e);
        });
    } else {
        emit eventReceived(e);
    }
}

// ---- Encoding / decoding ------------------------------------------

QByteArray InstanceHub::encodeFrame(const Event& e) {
    QByteArray body;
    {
        QDataStream ds(&body, QIODevice::WriteOnly);
        ds.setVersion(QDataStream::Qt_6_0);
        ds << e.seqno;
        ds << e.epoch;
        ds << static_cast<quint8>(e.tag);
        switch (e.tag) {
            case kMessagesAppended:
                ds << e.sessionId << e.messageId;
                break;
            case kBuffersChanged:
                ds << e.sessionId;
                break;
            case kSettingChanged:
                ds << e.settingName;
                break;
            default:
                break;
        }
    }
    QByteArray framed;
    {
        QDataStream ds(&framed, QIODevice::WriteOnly);
        ds.setVersion(QDataStream::Qt_6_0);
        ds << static_cast<qint32>(body.size());
    }
    framed.append(body);
    return framed;
}

void InstanceHub::decodeFrames(QByteArray& buf, QList<Event>& out) {
    while (true) {
        if (buf.size() < static_cast<int>(sizeof(qint32))) return;
        qint32 bodyLen = 0;
        {
            QDataStream ds(buf);
            ds.setVersion(QDataStream::Qt_6_0);
            ds >> bodyLen;
        }
        if (bodyLen < 0 || bodyLen > kMaxFrameBodyBytes) {
            // Malformed — drop the whole buffer. The caller will
            // close the socket (we can't close it here without
            // knowing which peer it is).
            buf.clear();
            return;
        }
        const int needed =
            static_cast<int>(sizeof(qint32)) + bodyLen;
        if (buf.size() < needed) return;

        const QByteArray body =
            buf.mid(static_cast<int>(sizeof(qint32)), bodyLen);
        buf.remove(0, needed);

        QDataStream ds(body);
        ds.setVersion(QDataStream::Qt_6_0);
        Event e;
        quint8 tagByte = 0;
        ds >> e.seqno >> e.epoch >> tagByte;
        e.tag = static_cast<EventTag>(tagByte);
        switch (e.tag) {
            case kMessagesAppended:
                ds >> e.sessionId >> e.messageId;
                break;
            case kBuffersChanged:
                ds >> e.sessionId;
                break;
            case kSettingChanged:
                ds >> e.settingName;
                break;
            default:
                break;
        }
        if (ds.status() == QDataStream::Ok) {
            out.append(e);
        }
    }
}

// ---- Leader-side socket handling ----------------------------------

void InstanceHub::onNewConnection() {
    while (m_server && m_server->hasPendingConnections()) {
        QLocalSocket* s = m_server->nextPendingConnection();
        Peer p;
        p.socket = s;
        m_peers.append(p);
        connect(s, &QLocalSocket::readyRead, this,
                &InstanceHub::onPeerReadyRead);
        connect(s, &QLocalSocket::disconnected, this,
                &InstanceHub::onPeerDisconnected);
        // Bring the new client up to date — announce our epoch
        // so the client latches it, then kResync so they re-query.
        Event announce;
        announce.tag = kLeaderAnnounce;
        announce.seqno = ++m_lastSeqno;
        announce.epoch = m_epoch;
        s->write(encodeFrame(announce));
        Event resync;
        resync.tag = kResync;
        resync.seqno = ++m_lastSeqno;
        resync.epoch = m_epoch;
        s->write(encodeFrame(resync));
    }
}

void InstanceHub::onPeerReadyRead() {
    auto* s = qobject_cast<QLocalSocket*>(sender());
    if (!s) return;
    for (auto& p : m_peers) {
        if (p.socket != s) continue;
        p.readBuf.append(s->readAll());
        QList<Event> events;
        decodeFrames(p.readBuf, events);
        for (const Event& inbound : events) {
            // Clients publish with seqno==0; stamp and re-fan.
            Event stamped = inbound;
            broadcastFromLeader(stamped);
        }
        return;
    }
}

void InstanceHub::onPeerDisconnected() {
    auto* s = qobject_cast<QLocalSocket*>(sender());
    if (!s) return;
    for (int i = 0; i < m_peers.size(); ++i) {
        if (m_peers[i].socket == s) {
            m_peers[i].socket->deleteLater();
            m_peers.removeAt(i);
            return;
        }
    }
}

// ---- Client-side socket handling ----------------------------------

void InstanceHub::onClientReadyRead() {
    if (!m_clientSocket) return;
    m_clientReadBuf.append(m_clientSocket->readAll());
    QList<Event> events;
    decodeFrames(m_clientReadBuf, events);
    QList<Event> accepted;
    for (const Event& e : events) {
        // Latch the highest epoch we've seen; ignore events from
        // a stale leader that hasn't finished shutting down.
        if (e.epoch > m_epoch) {
            m_epoch = e.epoch;
        } else if (e.epoch < m_epoch) {
            continue;
        }
        if (e.seqno > m_lastSeqno) {
            m_lastSeqno = e.seqno;
        }
        accepted.append(e);
    }
    // Emit the entire batch with one shared delay so that
    // relay jitter doesn't reorder events within a read.
    const int delay = relayDelayMs();
    if (delay > 0) {
        QTimer::singleShot(delay, this, [this, accepted]() {
            for (const Event& e : accepted) {
                emit eventReceived(e);
            }
        });
    } else {
        for (const Event& e : accepted) {
            emit eventReceived(e);
        }
    }
}

void InstanceHub::onClientDisconnected() {
    // Hub went away. Re-run election after exponential backoff
    // so we don't stampede with every other surviving client.
    m_clientSocket = nullptr;
    m_clientReadBuf.clear();
    ++m_electionAttempts;
    m_reelectTimer->start(electionCooldownMs());
}
