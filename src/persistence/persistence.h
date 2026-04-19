// SPDX-License-Identifier: GPL-3.0-only
//
// DAO singleton for the SQLite v1 schema. Every write goes through
// this class so the tree model, editor, and chat shell can react
// via signals. Not thread-safe — GUI thread only (a future worker
// thread will marshal writes back via QMetaObject::invokeMethod).

#pragma once

#include "instancehub.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QSet>
#include <QSqlDatabase>
#include <QString>
#include <optional>

struct ProjectRow {
    qint64 id = 0;
    std::optional<qint64> parentProjectId;
    QString name;
    QString rootPath;
    qint64 created = 0;
    qint64 updated = 0;
    qint64 lastUsed = 0;
};

struct SessionRow {
    qint64 id = 0;
    std::optional<qint64> projectId;
    QString title;
    QString cwd;
    QByteArray chatDraft;
    qint64 created = 0;
    qint64 updated = 0;
    qint64 lastUsed = 0;
};

struct BufferRow {
    qint64 id = 0;
    qint64 sessionId = 0;
    QString filePath;
    QByteArray dirtyContent;
    std::optional<qint64> savedMtime;
    qint64 opened = 0;
    int tabPosition = 0;
};

struct MessageRow {
    qint64 id = 0;
    qint64 sessionId = 0;
    QString msgRole;
    QByteArray content;
    qint64 created = 0;
};

struct AttachmentRow {
    qint64 id = 0;
    qint64 sessionId = 0;
    std::optional<qint64> messageId;  // NULL while scope == draft
    QString scope;  // "draft" or "message"
    QString uuid;
    QString filePath;  // path on disk under AppLocalDataLocation/attachments/
    QString mimeType;
    qint64 sizeBytes = 0;
    qint64 created = 0;
};

class Persistence : public QObject {
    Q_OBJECT
public:
    static Persistence& instance();

    // Wire InstanceHub::eventReceived → Persistence signal re-emit.
    // Called once from main() after both singletons are alive.
    void connectToHub();

    // Override the default AppLocalDataLocation-derived DB path. Must
    // be called before instance() for the first time. Intended for
    // unit tests that need an ephemeral DB under QTemporaryDir;
    // production code leaves this unset.
    static void setDatabasePathForTesting(const QString& absolutePath);

    // Inflate deferred signal timers to expose race conditions in
    // tests. Each QTimer::singleShot fires at a random delay in
    // [minMs, maxMs] instead of the production default of 0.
    // Call before instance() or in initTestCase().
    static void setDeferralJitterForTesting(int minMs, int maxMs);

    // Projects — can nest via parent_project_id
    qint64 createProject(std::optional<qint64> parentId,
                         const QString& name,
                         const QString& rootPath);
    void renameProject(qint64 id, const QString& newName);
    void reparentProject(qint64 id, std::optional<qint64> newParentId);
    void deleteProject(qint64 id);
    QList<ProjectRow> listProjects();
    QList<ProjectRow> listChildProjects(qint64 parentId);
    QList<ProjectRow> listTopLevelProjects();

    // Sessions
    qint64 createSession(std::optional<qint64> projectId,
                         const QString& title,
                         const QString& cwd);
    void renameSession(qint64 id, const QString& newTitle);
    void retargetSessionCwd(qint64 id, const QString& newCwd);
    void moveSessionToProject(qint64 id, std::optional<qint64> projectId);
    void deleteSession(qint64 id);

    // Touch `sessions.last_used` for `id` and propagate the touch
    // up the project ancestor chain (all `projects.last_used`
    // entries on the path to root) in a single transaction. This
    // is what drives the default `last_used DESC` sort order per
    // §5.1: activating a deeply-nested session floats its
    // enclosing projects to the top of their sibling lists.
    void activateSession(qint64 id);
    QList<SessionRow> listSessions();
    QList<SessionRow> listOrphanSessions();
    QList<SessionRow> listSessionsInProject(qint64 projectId);

    // Buffers
    qint64 openBuffer(qint64 sessionId, const QString& filePath = {});
    void closeBuffer(qint64 id);
    void saveBufferContent(qint64 id, const QByteArray& content, qint64 mtime);
    void stashDirtyBuffer(qint64 id, const QByteArray& content);
    void setBufferTabPosition(qint64 id, int position);
    std::optional<BufferRow> loadBuffer(qint64 id);
    QList<BufferRow> listBuffersForSession(qint64 sessionId);

    // Messages
    void appendMessage(qint64 sessionId, const QString& role, const QByteArray& content);
    QList<MessageRow> listMessagesForSession(qint64 sessionId);

    // Chat draft. Write-through persistence for in-progress chat
    // prompts so a power cut doesn't lose the user's halfway-
    // composed message.
    void setSessionChatDraft(qint64 sessionId, const QByteArray& draft);
    QByteArray sessionChatDraft(qint64 sessionId);

    // Atomic "append user message + clear draft". Used on submit
    // so a crash between the append and the clear can't end up
    // with the draft still present alongside the posted message.
    // Also promotes any draft-scoped attachments referenced by
    // the outgoing content to message scope (linking them to the
    // new message row) so they survive the draft clear.
    qint64 appendMessageAndClearDraft(qint64 sessionId,
                                      const QString& role,
                                      const QByteArray& content);

    // Attachments (paste-spill pathway). Attachments live on disk
    // under <AppLocalDataLocation>/attachments/<sessionId>/ and
    // are tracked here so session/message delete can cascade.
    qint64 insertDraftAttachment(qint64 sessionId,
                                 const QString& uuid,
                                 const QString& filePath,
                                 const QString& mimeType,
                                 qint64 sizeBytes);
    QList<AttachmentRow> listAttachmentsForSession(qint64 sessionId);
    QList<AttachmentRow> listDraftAttachments(qint64 sessionId);
    // Promote every draft-scoped attachment of `sessionId` to
    // message scope, attaching them to `messageId`. Called from
    // appendMessageAndClearDraft inside the transaction.
    void promoteDraftAttachments(qint64 sessionId, qint64 messageId);
    // Drop a single attachment row by uuid (called when the user
    // deletes the spill token from the draft). File cleanup is
    // the caller's job — the DAO owns the row, not the bytes.
    void deleteAttachmentByUuid(qint64 sessionId, const QString& uuid);

    // Settings KV — column is data_value to dodge SQL:2003 reserved `VALUE`.
    // setSetting with an empty value transparently routes to
    // clearSetting (the SQLite driver can't store an empty blob
    // under the data_value NOT NULL constraint, and readers treat
    // absent-row and empty-value identically anyway).
    void setSetting(const QString& name, const QByteArray& value);
    QByteArray getSetting(const QString& name, const QByteArray& fallback = {});
    void clearSetting(const QString& name);

signals:
    void projectsChanged();
    void sessionsChanged();
    void buffersChanged(qint64 sessionId);
    void messagesAppended(qint64 sessionId, qint64 messageId);
    void chatDraftChanged(qint64 sessionId);
    // Emitted after a settings_kv write lands (setSetting,
    // clearSetting). Subscribers wire this per key to live-apply
    // changes — e.g., EditorPaneWidget listens for `editor.*`
    // keys and reconfigures open editors without restart.
    void settingChanged(const QString& name);

private:
    Persistence();
    ~Persistence() override;
    Persistence(const Persistence&) = delete;
    Persistence& operator=(const Persistence&) = delete;

    // Re-entrant emits during a write were causing nested
    // beginResetModel/endResetModel cycles that left QTreeView
    // with invalidated persistent indexes. These helpers defer the
    // signal to the event loop via QTimer::singleShot(0), which
    // (a) unwinds the writing QSqlQuery before any slot runs and
    // (b) coalesces bursts (e.g. delete-project + promoted-orphan
    // cascade) into one model refresh.
    void scheduleProjectsChanged();
    void scheduleSessionsChanged();
    void scheduleSettingChanged(const QString& name);

    void publishProjectsChanged();
    void publishSessionsChanged();
    void publishBuffersChanged(qint64 sessionId);
    void publishMessagesAppended(qint64 sessionId, qint64 messageId);
    void publishSettingChanged(const QString& name);
    void publishChatDraftChanged(qint64 sessionId);

    void onHubEvent(const InstanceHub::Event& e);

    QString m_connectionName;
    QSqlDatabase m_db;

    bool m_projectsChangedPending = false;
    bool m_sessionsChangedPending = false;
    QSet<QString> m_pendingSettingNames;
};
