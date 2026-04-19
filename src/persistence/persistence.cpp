// SPDX-License-Identifier: GPL-3.0-only
#include "persistence.h"

#include "instancehub.h"
#include "migrations.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTimer>
#include <QVariant>
#include <stdexcept>

namespace {

QString& testDatabasePathRef() {
    static QString path;
    return path;
}

struct DeferralJitter {
    int minMs = 0;
    int maxMs = 0;
};

DeferralJitter& deferralJitterRef() {
    static DeferralJitter j;
    return j;
}

int deferralDelayMs() {
    const auto& j = deferralJitterRef();
    if (j.maxMs <= 0) return 0;
    return QRandomGenerator::global()->bounded(j.minMs, j.maxMs + 1);
}

QString defaultDatabasePath() {
    const QString dataDir =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(dataDir);
    return dataDir + QStringLiteral("/db.sqlite");
}

qint64 nowSecs() {
    return QDateTime::currentSecsSinceEpoch();
}

std::optional<qint64> toOptionalId(const QVariant& v) {
    if (v.isNull()) {
        return std::nullopt;
    }
    return v.toLongLong();
}

void bindOptionalId(QSqlQuery& q, const QString& placeholder,
                    const std::optional<qint64>& id) {
    if (id.has_value()) {
        q.bindValue(placeholder, QVariant::fromValue<qint64>(*id));
    } else {
        // Default-constructed QVariant is invalid → SQL NULL.
        q.bindValue(placeholder, QVariant());
    }
}

[[noreturn]] void fail(const QSqlQuery& q, const char* what) {
    throw std::runtime_error(
        (QString::fromUtf8(what) + QStringLiteral(": ") +
         q.lastError().text() + QStringLiteral(" — ") + q.lastQuery())
            .toStdString());
}

ProjectRow rowToProject(const QSqlQuery& q) {
    ProjectRow r;
    r.id = q.value(0).toLongLong();
    r.parentProjectId = toOptionalId(q.value(1));
    r.name = q.value(2).toString();
    r.rootPath = q.value(3).toString();
    r.created = q.value(4).toLongLong();
    r.updated = q.value(5).toLongLong();
    r.lastUsed = q.value(6).toLongLong();
    return r;
}

SessionRow rowToSession(const QSqlQuery& q) {
    SessionRow r;
    r.id = q.value(0).toLongLong();
    r.projectId = toOptionalId(q.value(1));
    r.title = q.value(2).toString();
    r.cwd = q.value(3).toString();
    r.chatDraft = q.value(4).toByteArray();
    r.created = q.value(5).toLongLong();
    r.updated = q.value(6).toLongLong();
    r.lastUsed = q.value(7).toLongLong();
    return r;
}

BufferRow rowToBuffer(const QSqlQuery& q) {
    BufferRow r;
    r.id = q.value(0).toLongLong();
    r.sessionId = q.value(1).toLongLong();
    r.filePath = q.value(2).toString();
    r.dirtyContent = q.value(3).toByteArray();
    if (!q.value(4).isNull()) {
        r.savedMtime = q.value(4).toLongLong();
    }
    r.opened = q.value(5).toLongLong();
    r.tabPosition = q.value(6).toInt();
    return r;
}

MessageRow rowToMessage(const QSqlQuery& q) {
    MessageRow r;
    r.id = q.value(0).toLongLong();
    r.sessionId = q.value(1).toLongLong();
    r.msgRole = q.value(2).toString();
    r.content = q.value(3).toByteArray();
    r.created = q.value(4).toLongLong();
    return r;
}

AttachmentRow rowToAttachment(const QSqlQuery& q) {
    AttachmentRow r;
    r.id = q.value(0).toLongLong();
    r.sessionId = q.value(1).toLongLong();
    r.messageId = toOptionalId(q.value(2));
    r.scope = q.value(3).toString();
    r.uuid = q.value(4).toString();
    r.filePath = q.value(5).toString();
    r.mimeType = q.value(6).toString();
    r.sizeBytes = q.value(7).toLongLong();
    r.created = q.value(8).toLongLong();
    return r;
}

} // namespace

void Persistence::setDatabasePathForTesting(const QString& absolutePath) {
    testDatabasePathRef() = absolutePath;
}

void Persistence::setDeferralJitterForTesting(int minMs, int maxMs) {
    auto& j = deferralJitterRef();
    j.minMs = minMs;
    j.maxMs = maxMs;
}

Persistence& Persistence::instance() {
    static Persistence s;
    return s;
}

Persistence::Persistence() {
    const QString appName = QCoreApplication::applicationName();
    m_connectionName = (appName.isEmpty() ? QStringLiteral("app") : appName) +
                       QStringLiteral("_main");

    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    const QString path = testDatabasePathRef().isEmpty()
                             ? defaultDatabasePath()
                             : testDatabasePathRef();
    m_db.setDatabaseName(path);
    if (!m_db.open()) {
        throw std::runtime_error(
            (QStringLiteral("failed to open DB at ") + path +
             QStringLiteral(": ") + m_db.lastError().text())
                .toStdString());
    }

    QSqlQuery pragma(m_db);
    pragma.exec(QStringLiteral("PRAGMA journal_mode = WAL"));
    pragma.exec(QStringLiteral("PRAGMA synchronous = NORMAL"));
    pragma.exec(QStringLiteral("PRAGMA foreign_keys = ON"));
    pragma.exec(QStringLiteral("PRAGMA busy_timeout = 5000"));

    Migrations::runAll(m_db);
}

Persistence::~Persistence() {
    if (m_db.isOpen()) {
        m_db.close();
    }
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase(m_connectionName);
}

void Persistence::connectToHub() {
    connect(&InstanceHub::instance(), &InstanceHub::eventReceived,
            this, &Persistence::onHubEvent);
}

void Persistence::onHubEvent(const InstanceHub::Event& e) {
    switch (e.tag) {
        case InstanceHub::kProjectsChanged:
            scheduleProjectsChanged();
            break;
        case InstanceHub::kSessionsChanged:
            scheduleSessionsChanged();
            break;
        case InstanceHub::kBuffersChanged:
            emit buffersChanged(e.sessionId);
            break;
        case InstanceHub::kMessagesAppended:
            emit messagesAppended(e.sessionId, e.messageId);
            break;
        case InstanceHub::kDraftChanged:
            emit chatDraftChanged(e.sessionId);
            break;
        case InstanceHub::kSettingChanged:
            scheduleSettingChanged(e.settingName);
            break;
        case InstanceHub::kResync:
            scheduleProjectsChanged();
            scheduleSessionsChanged();
            break;
        case InstanceHub::kLeaderAnnounce:
            break;
    }
}

// -----------------------------------------------------------------
// Projects
// -----------------------------------------------------------------

qint64 Persistence::createProject(std::optional<qint64> parentId,
                                  const QString& name,
                                  const QString& rootPath) {
    const qint64 now = nowSecs();
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO projects "
        "(parent_project_id, name, root_path, created, updated, last_used) "
        "VALUES (:parent, :name, :root, :c, :u, :lu)"));
    bindOptionalId(q, QStringLiteral(":parent"), parentId);
    q.bindValue(QStringLiteral(":name"), name);
    q.bindValue(QStringLiteral(":root"), rootPath);
    q.bindValue(QStringLiteral(":c"), QVariant::fromValue<qint64>(now));
    q.bindValue(QStringLiteral(":u"), QVariant::fromValue<qint64>(now));
    q.bindValue(QStringLiteral(":lu"), QVariant::fromValue<qint64>(now));
    if (!q.exec()) {
        fail(q, "createProject insert failed");
    }
    const qint64 id = q.lastInsertId().toLongLong();
    publishProjectsChanged();
    return id;
}

void Persistence::renameProject(qint64 id, const QString& newName) {
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "UPDATE projects SET name = :name, updated = :u WHERE id = :id"));
    q.bindValue(QStringLiteral(":name"), newName);
    q.bindValue(QStringLiteral(":u"), QVariant::fromValue<qint64>(nowSecs()));
    q.bindValue(QStringLiteral(":id"), QVariant::fromValue<qint64>(id));
    if (!q.exec()) {
        fail(q, "renameProject failed");
    }
    publishProjectsChanged();
}

void Persistence::reparentProject(qint64 id,
                                  std::optional<qint64> newParentId) {
    // Cycle guard: walk the ancestor chain of newParentId and refuse if
    // we hit `id`. Top-level (nullopt) is trivially cycle-free.
    if (newParentId.has_value()) {
        std::optional<qint64> cursor = newParentId;
        while (cursor.has_value()) {
            if (*cursor == id) {
                throw std::runtime_error(
                    "reparentProject would create a cycle");
            }
            QSqlQuery probe(m_db);
            probe.prepare(QStringLiteral(
                "SELECT parent_project_id FROM projects WHERE id = :id"));
            probe.bindValue(QStringLiteral(":id"),
                            QVariant::fromValue<qint64>(*cursor));
            if (!probe.exec() || !probe.next()) {
                fail(probe, "reparentProject cycle probe failed");
            }
            cursor = toOptionalId(probe.value(0));
        }
    }

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "UPDATE projects SET parent_project_id = :p, updated = :u "
        "WHERE id = :id"));
    bindOptionalId(q, QStringLiteral(":p"), newParentId);
    q.bindValue(QStringLiteral(":u"), QVariant::fromValue<qint64>(nowSecs()));
    q.bindValue(QStringLiteral(":id"), QVariant::fromValue<qint64>(id));
    if (!q.exec()) {
        fail(q, "reparentProject failed");
    }
    publishProjectsChanged();
}

void Persistence::deleteProject(qint64 id) {
    // ON DELETE SET NULL on projects.parent_project_id promotes children
    // to top-level automatically; same for sessions.project_id → orphans.
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM projects WHERE id = :id"));
    q.bindValue(QStringLiteral(":id"), QVariant::fromValue<qint64>(id));
    if (!q.exec()) {
        fail(q, "deleteProject failed");
    }
    publishProjectsChanged();
    publishSessionsChanged();
}

QList<ProjectRow> Persistence::listProjects() {
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral(
            "SELECT id, parent_project_id, name, root_path, "
            "       created, updated, last_used "
            "FROM projects"))) {
        fail(q, "listProjects failed");
    }
    QList<ProjectRow> out;
    while (q.next()) {
        out.append(rowToProject(q));
    }
    return out;
}

QList<ProjectRow> Persistence::listChildProjects(qint64 parentId) {
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT id, parent_project_id, name, root_path, "
        "       created, updated, last_used "
        "FROM projects WHERE parent_project_id = :p ORDER BY name"));
    q.bindValue(QStringLiteral(":p"), QVariant::fromValue<qint64>(parentId));
    if (!q.exec()) {
        fail(q, "listChildProjects failed");
    }
    QList<ProjectRow> out;
    while (q.next()) {
        out.append(rowToProject(q));
    }
    return out;
}

QList<ProjectRow> Persistence::listTopLevelProjects() {
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral(
            "SELECT id, parent_project_id, name, root_path, "
            "       created, updated, last_used "
            "FROM projects WHERE parent_project_id IS NULL ORDER BY name"))) {
        fail(q, "listTopLevelProjects failed");
    }
    QList<ProjectRow> out;
    while (q.next()) {
        out.append(rowToProject(q));
    }
    return out;
}

// -----------------------------------------------------------------
// Sessions
// -----------------------------------------------------------------

qint64 Persistence::createSession(std::optional<qint64> projectId,
                                  const QString& title,
                                  const QString& cwd) {
    const qint64 now = nowSecs();
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO sessions "
        "(project_id, title, cwd, chat_draft, created, updated, last_used) "
        "VALUES (:p, :t, :cwd, NULL, :c, :u, :lu)"));
    bindOptionalId(q, QStringLiteral(":p"), projectId);
    q.bindValue(QStringLiteral(":t"), title);
    q.bindValue(QStringLiteral(":cwd"), cwd);
    q.bindValue(QStringLiteral(":c"), QVariant::fromValue<qint64>(now));
    q.bindValue(QStringLiteral(":u"), QVariant::fromValue<qint64>(now));
    q.bindValue(QStringLiteral(":lu"), QVariant::fromValue<qint64>(now));
    if (!q.exec()) {
        fail(q, "createSession insert failed");
    }
    const qint64 id = q.lastInsertId().toLongLong();
    publishSessionsChanged();
    return id;
}

void Persistence::renameSession(qint64 id, const QString& newTitle) {
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "UPDATE sessions SET title = :t, updated = :u WHERE id = :id"));
    q.bindValue(QStringLiteral(":t"), newTitle);
    q.bindValue(QStringLiteral(":u"), QVariant::fromValue<qint64>(nowSecs()));
    q.bindValue(QStringLiteral(":id"), QVariant::fromValue<qint64>(id));
    if (!q.exec()) {
        fail(q, "renameSession failed");
    }
    publishSessionsChanged();
}

void Persistence::retargetSessionCwd(qint64 id, const QString& newCwd) {
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "UPDATE sessions SET cwd = :cwd, updated = :u WHERE id = :id"));
    q.bindValue(QStringLiteral(":cwd"), newCwd);
    q.bindValue(QStringLiteral(":u"), QVariant::fromValue<qint64>(nowSecs()));
    q.bindValue(QStringLiteral(":id"), QVariant::fromValue<qint64>(id));
    if (!q.exec()) {
        fail(q, "retargetSessionCwd failed");
    }
    publishSessionsChanged();
}

void Persistence::moveSessionToProject(qint64 id,
                                       std::optional<qint64> projectId) {
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "UPDATE sessions SET project_id = :p, updated = :u WHERE id = :id"));
    bindOptionalId(q, QStringLiteral(":p"), projectId);
    q.bindValue(QStringLiteral(":u"), QVariant::fromValue<qint64>(nowSecs()));
    q.bindValue(QStringLiteral(":id"), QVariant::fromValue<qint64>(id));
    if (!q.exec()) {
        fail(q, "moveSessionToProject failed");
    }
    publishSessionsChanged();
}

void Persistence::activateSession(qint64 id) {
    // §5.1: touch session.last_used, then walk up the project
    // chain touching every ancestor. Wrapped in a transaction so
    // a concurrent reader never sees a half-propagated update.
    const qint64 ts = nowSecs();

    if (!m_db.transaction()) {
        throw std::runtime_error(
            ("activateSession: begin transaction failed: " +
             m_db.lastError().text())
                .toStdString());
    }
    try {
        QSqlQuery q(m_db);
        q.prepare(QStringLiteral(
            "UPDATE sessions SET last_used = :u WHERE id = :id"));
        q.bindValue(QStringLiteral(":u"), QVariant::fromValue<qint64>(ts));
        q.bindValue(QStringLiteral(":id"), QVariant::fromValue<qint64>(id));
        if (!q.exec()) {
            fail(q, "activateSession: session touch failed");
        }

        // Find the session's project_id; bail cleanly if the
        // session is an orphan.
        QSqlQuery lookup(m_db);
        lookup.prepare(QStringLiteral(
            "SELECT project_id FROM sessions WHERE id = :id"));
        lookup.bindValue(QStringLiteral(":id"),
                         QVariant::fromValue<qint64>(id));
        if (!lookup.exec() || !lookup.next()) {
            fail(lookup, "activateSession: session lookup failed");
        }
        std::optional<qint64> cursor = toOptionalId(lookup.value(0));

        // Walk ancestors. reparentProject's cycle guard ensures
        // the graph is a DAG, so this terminates.
        while (cursor.has_value()) {
            QSqlQuery up(m_db);
            up.prepare(QStringLiteral(
                "UPDATE projects SET last_used = :u WHERE id = :id"));
            up.bindValue(QStringLiteral(":u"),
                         QVariant::fromValue<qint64>(ts));
            up.bindValue(QStringLiteral(":id"),
                         QVariant::fromValue<qint64>(*cursor));
            if (!up.exec()) {
                fail(up, "activateSession: project touch failed");
            }

            QSqlQuery probe(m_db);
            probe.prepare(QStringLiteral(
                "SELECT parent_project_id FROM projects WHERE id = :id"));
            probe.bindValue(QStringLiteral(":id"),
                            QVariant::fromValue<qint64>(*cursor));
            if (!probe.exec() || !probe.next()) {
                fail(probe, "activateSession: ancestor probe failed");
            }
            cursor = toOptionalId(probe.value(0));
        }
    } catch (...) {
        m_db.rollback();
        throw;
    }
    if (!m_db.commit()) {
        throw std::runtime_error(
            ("activateSession: commit failed: " +
             m_db.lastError().text())
                .toStdString());
    }
    publishSessionsChanged();
    publishProjectsChanged();
}

void Persistence::deleteSession(qint64 id) {
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM sessions WHERE id = :id"));
    q.bindValue(QStringLiteral(":id"), QVariant::fromValue<qint64>(id));
    if (!q.exec()) {
        fail(q, "deleteSession failed");
    }
    publishSessionsChanged();
}

QList<SessionRow> Persistence::listSessions() {
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral(
            "SELECT id, project_id, title, cwd, chat_draft, "
            "       created, updated, last_used "
            "FROM sessions"))) {
        fail(q, "listSessions failed");
    }
    QList<SessionRow> out;
    while (q.next()) {
        out.append(rowToSession(q));
    }
    return out;
}

QList<SessionRow> Persistence::listOrphanSessions() {
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral(
            "SELECT id, project_id, title, cwd, chat_draft, "
            "       created, updated, last_used "
            "FROM sessions WHERE project_id IS NULL "
            "ORDER BY last_used DESC"))) {
        fail(q, "listOrphanSessions failed");
    }
    QList<SessionRow> out;
    while (q.next()) {
        out.append(rowToSession(q));
    }
    return out;
}

QList<SessionRow> Persistence::listSessionsInProject(qint64 projectId) {
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT id, project_id, title, cwd, chat_draft, "
        "       created, updated, last_used "
        "FROM sessions WHERE project_id = :p "
        "ORDER BY last_used DESC"));
    q.bindValue(QStringLiteral(":p"), QVariant::fromValue<qint64>(projectId));
    if (!q.exec()) {
        fail(q, "listSessionsInProject failed");
    }
    QList<SessionRow> out;
    while (q.next()) {
        out.append(rowToSession(q));
    }
    return out;
}

// -----------------------------------------------------------------
// Buffers
// -----------------------------------------------------------------

qint64 Persistence::openBuffer(qint64 sessionId, const QString& filePath) {
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO buffers (session_id, file_path, dirty_content, "
        "                     saved_mtime, opened) "
        "VALUES (:s, :f, NULL, NULL, :o)"));
    q.bindValue(QStringLiteral(":s"), QVariant::fromValue<qint64>(sessionId));
    if (filePath.isEmpty()) {
        q.bindValue(QStringLiteral(":f"), QVariant(QMetaType(QMetaType::QString)));
    } else {
        q.bindValue(QStringLiteral(":f"), filePath);
    }
    q.bindValue(QStringLiteral(":o"), QVariant::fromValue<qint64>(nowSecs()));
    if (!q.exec()) {
        fail(q, "openBuffer failed");
    }
    const qint64 id = q.lastInsertId().toLongLong();
    publishBuffersChanged(sessionId);
    return id;
}

void Persistence::closeBuffer(qint64 id) {
    QSqlQuery pre(m_db);
    pre.prepare(QStringLiteral("SELECT session_id FROM buffers WHERE id = :id"));
    pre.bindValue(QStringLiteral(":id"), QVariant::fromValue<qint64>(id));
    qint64 sessionId = 0;
    if (pre.exec() && pre.next()) {
        sessionId = pre.value(0).toLongLong();
    }

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM buffers WHERE id = :id"));
    q.bindValue(QStringLiteral(":id"), QVariant::fromValue<qint64>(id));
    if (!q.exec()) {
        fail(q, "closeBuffer failed");
    }
    publishBuffersChanged(sessionId);
}

void Persistence::saveBufferContent(qint64 id, const QByteArray& content,
                                    qint64 mtime) {
    // "save" here means the in-flight content was flushed to disk by the
    // editor — the DB record clears dirty_content and records mtime so a
    // subsequent stat() check can detect external modifications.
    Q_UNUSED(content);
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "UPDATE buffers SET dirty_content = NULL, saved_mtime = :m "
        "WHERE id = :id"));
    q.bindValue(QStringLiteral(":m"), QVariant::fromValue<qint64>(mtime));
    q.bindValue(QStringLiteral(":id"), QVariant::fromValue<qint64>(id));
    if (!q.exec()) {
        fail(q, "saveBufferContent failed");
    }
}

void Persistence::stashDirtyBuffer(qint64 id, const QByteArray& content) {
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "UPDATE buffers SET dirty_content = :c WHERE id = :id"));
    q.bindValue(QStringLiteral(":c"), content);
    q.bindValue(QStringLiteral(":id"), QVariant::fromValue<qint64>(id));
    if (!q.exec()) {
        fail(q, "stashDirtyBuffer failed");
    }

    QSqlQuery sid(m_db);
    sid.prepare(QStringLiteral(
        "SELECT session_id FROM buffers WHERE id = :id"));
    sid.bindValue(QStringLiteral(":id"), QVariant::fromValue<qint64>(id));
    if (sid.exec() && sid.next()) {
        publishBuffersChanged(sid.value(0).toLongLong());
    }
}

void Persistence::setBufferTabPosition(qint64 id, int position) {
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "UPDATE buffers SET tab_position = :p WHERE id = :id"));
    q.bindValue(QStringLiteral(":p"), position);
    q.bindValue(QStringLiteral(":id"), QVariant::fromValue<qint64>(id));
    if (!q.exec()) {
        fail(q, "setBufferTabPosition failed");
    }

    QSqlQuery sid(m_db);
    sid.prepare(QStringLiteral(
        "SELECT session_id FROM buffers WHERE id = :id"));
    sid.bindValue(QStringLiteral(":id"), QVariant::fromValue<qint64>(id));
    if (sid.exec() && sid.next()) {
        publishBuffersChanged(sid.value(0).toLongLong());
    }
}

std::optional<BufferRow> Persistence::loadBuffer(qint64 id) {
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT id, session_id, file_path, dirty_content, saved_mtime, "
        "opened, tab_position FROM buffers WHERE id = :id"));
    q.bindValue(QStringLiteral(":id"), QVariant::fromValue<qint64>(id));
    if (!q.exec()) {
        fail(q, "loadBuffer failed");
    }
    if (!q.next()) {
        return std::nullopt;
    }
    return rowToBuffer(q);
}

QList<BufferRow> Persistence::listBuffersForSession(qint64 sessionId) {
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT id, session_id, file_path, dirty_content, saved_mtime, "
        "opened, tab_position FROM buffers WHERE session_id = :s "
        "ORDER BY tab_position, opened"));
    q.bindValue(QStringLiteral(":s"), QVariant::fromValue<qint64>(sessionId));
    if (!q.exec()) {
        fail(q, "listBuffersForSession failed");
    }
    QList<BufferRow> out;
    while (q.next()) {
        out.append(rowToBuffer(q));
    }
    return out;
}

// -----------------------------------------------------------------
// Messages
// -----------------------------------------------------------------

void Persistence::appendMessage(qint64 sessionId, const QString& role,
                                const QByteArray& content) {
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO messages (session_id, msg_role, content, created) "
        "VALUES (:s, :r, :c, :t)"));
    q.bindValue(QStringLiteral(":s"), QVariant::fromValue<qint64>(sessionId));
    q.bindValue(QStringLiteral(":r"), role);
    q.bindValue(QStringLiteral(":c"), content);
    q.bindValue(QStringLiteral(":t"), QVariant::fromValue<qint64>(nowSecs()));
    if (!q.exec()) {
        fail(q, "appendMessage failed");
    }
    const qint64 newMsgId = q.lastInsertId().toLongLong();
    publishMessagesAppended(sessionId, newMsgId);
}

QList<MessageRow> Persistence::listMessagesForSession(qint64 sessionId) {
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT id, session_id, msg_role, content, created "
        "FROM messages WHERE session_id = :s ORDER BY created, id"));
    q.bindValue(QStringLiteral(":s"), QVariant::fromValue<qint64>(sessionId));
    if (!q.exec()) {
        fail(q, "listMessagesForSession failed");
    }
    QList<MessageRow> out;
    while (q.next()) {
        out.append(rowToMessage(q));
    }
    return out;
}

// -----------------------------------------------------------------
// Chat draft
// -----------------------------------------------------------------

void Persistence::setSessionChatDraft(qint64 sessionId,
                                      const QByteArray& draft) {
    // An empty draft writes SQL NULL (the `chat_draft` column is
    // nullable and the CHECK constraint explicitly allows NULL).
    // Qt's SQLite driver binds empty QByteArray as SQL NULL which
    // is exactly what we want here, unlike the settings_kv case
    // where NOT NULL is enforced.
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "UPDATE sessions SET chat_draft = :d, updated = :u WHERE id = :id"));
    if (draft.isEmpty()) {
        q.bindValue(QStringLiteral(":d"), QVariant(QMetaType(QMetaType::QByteArray)));
    } else {
        q.bindValue(QStringLiteral(":d"), draft);
    }
    q.bindValue(QStringLiteral(":u"), QVariant::fromValue<qint64>(nowSecs()));
    q.bindValue(QStringLiteral(":id"), QVariant::fromValue<qint64>(sessionId));
    if (!q.exec()) {
        fail(q, "setSessionChatDraft failed");
    }
    publishChatDraftChanged(sessionId);
}

QByteArray Persistence::sessionChatDraft(qint64 sessionId) {
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT chat_draft FROM sessions WHERE id = :id"));
    q.bindValue(QStringLiteral(":id"), QVariant::fromValue<qint64>(sessionId));
    if (!q.exec() || !q.next()) return {};
    return q.value(0).toByteArray();
}

qint64 Persistence::appendMessageAndClearDraft(qint64 sessionId,
                                               const QString& role,
                                               const QByteArray& content) {
    // Atomic append + clear per §7.3. Wraps two writes in a
    // transaction so a crash mid-operation can't leave the draft
    // still present alongside the already-inserted message row.
    if (!m_db.transaction()) {
        fail(QSqlQuery(m_db), "appendMessageAndClearDraft: transaction open failed");
    }
    qint64 messageId = 0;
    try {
        QSqlQuery ins(m_db);
        ins.prepare(QStringLiteral(
            "INSERT INTO messages (session_id, msg_role, content, created) "
            "VALUES (:s, :r, :c, :t)"));
        ins.bindValue(QStringLiteral(":s"), QVariant::fromValue<qint64>(sessionId));
        ins.bindValue(QStringLiteral(":r"), role);
        ins.bindValue(QStringLiteral(":c"), content);
        ins.bindValue(QStringLiteral(":t"), QVariant::fromValue<qint64>(nowSecs()));
        if (!ins.exec()) {
            fail(ins, "appendMessageAndClearDraft: insert message failed");
        }
        messageId = ins.lastInsertId().toLongLong();

        QSqlQuery clr(m_db);
        clr.prepare(QStringLiteral(
            "UPDATE sessions SET chat_draft = NULL, updated = :u WHERE id = :id"));
        clr.bindValue(QStringLiteral(":u"), QVariant::fromValue<qint64>(nowSecs()));
        clr.bindValue(QStringLiteral(":id"), QVariant::fromValue<qint64>(sessionId));
        if (!clr.exec()) {
            fail(clr, "appendMessageAndClearDraft: clear draft failed");
        }

        // Promote any draft-scoped attachments to message scope
        // so the cascade keys survive the draft clear.
        QSqlQuery prom(m_db);
        prom.prepare(QStringLiteral(
            "UPDATE attachments SET attachment_scope = 'message', "
            "message_id = :mid "
            "WHERE session_id = :sid AND attachment_scope = 'draft'"));
        prom.bindValue(QStringLiteral(":mid"), QVariant::fromValue<qint64>(messageId));
        prom.bindValue(QStringLiteral(":sid"), QVariant::fromValue<qint64>(sessionId));
        if (!prom.exec()) {
            fail(prom, "appendMessageAndClearDraft: promote attachments failed");
        }
    } catch (...) {
        m_db.rollback();
        throw;
    }
    if (!m_db.commit()) {
        throw std::runtime_error(
            (QStringLiteral("appendMessageAndClearDraft commit failed: ") +
             m_db.lastError().text()).toStdString());
    }
    publishMessagesAppended(sessionId, messageId);
    return messageId;
}

// -----------------------------------------------------------------
// Attachments (paste-spill pathway)
// -----------------------------------------------------------------

qint64 Persistence::insertDraftAttachment(qint64 sessionId,
                                          const QString& uuid,
                                          const QString& filePath,
                                          const QString& mimeType,
                                          qint64 sizeBytes) {
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO attachments "
        "(session_id, message_id, attachment_scope, uuid, file_path, "
        " mime_type, size_bytes, created) "
        "VALUES (:s, NULL, 'draft', :u, :f, :m, :sz, :c)"));
    q.bindValue(QStringLiteral(":s"), QVariant::fromValue<qint64>(sessionId));
    q.bindValue(QStringLiteral(":u"), uuid);
    q.bindValue(QStringLiteral(":f"), filePath);
    q.bindValue(QStringLiteral(":m"), mimeType);
    q.bindValue(QStringLiteral(":sz"), QVariant::fromValue<qint64>(sizeBytes));
    q.bindValue(QStringLiteral(":c"), QVariant::fromValue<qint64>(nowSecs()));
    if (!q.exec()) {
        fail(q, "insertDraftAttachment failed");
    }
    return q.lastInsertId().toLongLong();
}

QList<AttachmentRow> Persistence::listAttachmentsForSession(qint64 sessionId) {
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT id, session_id, message_id, attachment_scope, uuid, "
        "       file_path, mime_type, size_bytes, created "
        "FROM attachments WHERE session_id = :s ORDER BY created, id"));
    q.bindValue(QStringLiteral(":s"), QVariant::fromValue<qint64>(sessionId));
    if (!q.exec()) {
        fail(q, "listAttachmentsForSession failed");
    }
    QList<AttachmentRow> out;
    while (q.next()) {
        out.append(rowToAttachment(q));
    }
    return out;
}

QList<AttachmentRow> Persistence::listDraftAttachments(qint64 sessionId) {
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT id, session_id, message_id, attachment_scope, uuid, "
        "       file_path, mime_type, size_bytes, created "
        "FROM attachments WHERE session_id = :s "
        "  AND attachment_scope = 'draft' "
        "ORDER BY created, id"));
    q.bindValue(QStringLiteral(":s"), QVariant::fromValue<qint64>(sessionId));
    if (!q.exec()) {
        fail(q, "listDraftAttachments failed");
    }
    QList<AttachmentRow> out;
    while (q.next()) {
        out.append(rowToAttachment(q));
    }
    return out;
}

void Persistence::promoteDraftAttachments(qint64 sessionId, qint64 messageId) {
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "UPDATE attachments SET attachment_scope = 'message', "
        "message_id = :mid "
        "WHERE session_id = :sid AND attachment_scope = 'draft'"));
    q.bindValue(QStringLiteral(":mid"), QVariant::fromValue<qint64>(messageId));
    q.bindValue(QStringLiteral(":sid"), QVariant::fromValue<qint64>(sessionId));
    if (!q.exec()) {
        fail(q, "promoteDraftAttachments failed");
    }
}

void Persistence::deleteAttachmentByUuid(qint64 sessionId,
                                         const QString& uuid) {
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "DELETE FROM attachments WHERE session_id = :s AND uuid = :u"));
    q.bindValue(QStringLiteral(":s"), QVariant::fromValue<qint64>(sessionId));
    q.bindValue(QStringLiteral(":u"), uuid);
    if (!q.exec()) {
        fail(q, "deleteAttachmentByUuid failed");
    }
}

// -----------------------------------------------------------------
// Settings KV
// -----------------------------------------------------------------

void Persistence::setSetting(const QString& name, const QByteArray& value) {
    // Qt's SQLite driver binds an empty QByteArray as SQL NULL,
    // which violates the settings_kv.data_value NOT NULL
    // constraint. Route empty values through clearSetting —
    // every reader treats "row missing" and "row present but
    // empty" identically (falls back to default) so the two are
    // semantically equivalent, and this spares every caller from
    // having to remember the footgun.
    if (value.isEmpty()) {
        clearSetting(name);
        return;
    }
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO settings_kv (name, data_value) VALUES (:n, :v) "
        "ON CONFLICT(name) DO UPDATE SET data_value = excluded.data_value"));
    q.bindValue(QStringLiteral(":n"), name);
    q.bindValue(QStringLiteral(":v"), value);
    if (!q.exec()) {
        fail(q, "setSetting failed");
    }
    publishSettingChanged(name);
}

QByteArray Persistence::getSetting(const QString& name,
                                   const QByteArray& fallback) {
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT data_value FROM settings_kv WHERE name = :n"));
    q.bindValue(QStringLiteral(":n"), name);
    if (!q.exec()) {
        fail(q, "getSetting failed");
    }
    if (!q.next()) {
        return fallback;
    }
    return q.value(0).toByteArray();
}

void Persistence::clearSetting(const QString& name) {
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "DELETE FROM settings_kv WHERE name = :n"));
    q.bindValue(QStringLiteral(":n"), name);
    if (!q.exec()) {
        fail(q, "clearSetting failed");
    }
    publishSettingChanged(name);
}

// -----------------------------------------------------------------
// Signal coalescing (see persistence.h notes)
// -----------------------------------------------------------------

void Persistence::scheduleProjectsChanged() {
    if (m_projectsChangedPending) return;
    m_projectsChangedPending = true;
    QTimer::singleShot(deferralDelayMs(), this, [this]() {
        m_projectsChangedPending = false;
        emit projectsChanged();
    });
}

void Persistence::scheduleSessionsChanged() {
    if (m_sessionsChangedPending) return;
    m_sessionsChangedPending = true;
    QTimer::singleShot(deferralDelayMs(), this, [this]() {
        m_sessionsChangedPending = false;
        emit sessionsChanged();
    });
}

void Persistence::scheduleSettingChanged(const QString& name) {
    const bool wasEmpty = m_pendingSettingNames.isEmpty();
    m_pendingSettingNames.insert(name);
    if (!wasEmpty) return;
    QTimer::singleShot(deferralDelayMs(), this, [this]() {
        const QSet<QString> names = std::move(m_pendingSettingNames);
        m_pendingSettingNames.clear();
        for (const QString& n : names) {
            emit settingChanged(n);
        }
    });
}

// -----------------------------------------------------------------
// Hub publish helpers — DAO writes go through these so that all
// signals flow back through onHubEvent as the single delivery
// path. The hub stamps a seqno and echoes the event; onHubEvent
// calls the schedule* helpers above to fire the actual Qt signal.
// -----------------------------------------------------------------

void Persistence::publishProjectsChanged() {
    InstanceHub::Event ev;
    ev.tag = InstanceHub::kProjectsChanged;
    InstanceHub::instance().publish(ev);
}

void Persistence::publishSessionsChanged() {
    InstanceHub::Event ev;
    ev.tag = InstanceHub::kSessionsChanged;
    InstanceHub::instance().publish(ev);
}

void Persistence::publishBuffersChanged(qint64 sessionId) {
    InstanceHub::Event ev;
    ev.tag = InstanceHub::kBuffersChanged;
    ev.sessionId = sessionId;
    InstanceHub::instance().publish(ev);
}

void Persistence::publishMessagesAppended(qint64 sessionId,
                                          qint64 messageId) {
    InstanceHub::Event ev;
    ev.tag = InstanceHub::kMessagesAppended;
    ev.sessionId = sessionId;
    ev.messageId = messageId;
    InstanceHub::instance().publish(ev);
}

void Persistence::publishChatDraftChanged(qint64 sessionId) {
    InstanceHub::Event ev;
    ev.tag = InstanceHub::kDraftChanged;
    ev.sessionId = sessionId;
    InstanceHub::instance().publish(ev);
}

void Persistence::publishSettingChanged(const QString& name) {
    InstanceHub::Event ev;
    ev.tag = InstanceHub::kSettingChanged;
    ev.settingName = name;
    InstanceHub::instance().publish(ev);
}
