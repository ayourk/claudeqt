// SPDX-License-Identifier: GPL-3.0-only
#include "migrations.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <stdexcept>

namespace {

void exec(QSqlQuery& q, const QString& sql) {
    if (!q.exec(sql)) {
        throw std::runtime_error(
            (QStringLiteral("migration SQL failed: ") +
             q.lastError().text() + QStringLiteral(" [") + sql +
             QStringLiteral("]"))
                .toStdString());
    }
}

} // namespace

int Migrations::currentVersion(QSqlDatabase& db) {
    QSqlQuery q(db);
    // If schema_version doesn't exist yet, treat as v0.
    if (!q.exec(QStringLiteral(
            "SELECT name FROM sqlite_master "
            "WHERE type='table' AND name='schema_version'"))) {
        throw std::runtime_error(
            QStringLiteral("schema probe failed: ").toStdString() +
            q.lastError().text().toStdString());
    }
    if (!q.next()) {
        return 0;
    }
    if (!q.exec(QStringLiteral("SELECT version FROM schema_version"))) {
        throw std::runtime_error(
            QStringLiteral("schema_version read failed: ").toStdString() +
            q.lastError().text().toStdString());
    }
    if (q.next()) {
        return q.value(0).toInt();
    }
    return 0;
}

void Migrations::runAll(QSqlDatabase& db) {
    const int start = currentVersion(db);
    if (start >= kLatestVersion) {
        return;
    }

    for (int v = start; v < kLatestVersion; ++v) {
        if (!db.transaction()) {
            throw std::runtime_error(
                QStringLiteral("migration transaction failed to start: ")
                    .toStdString() +
                db.lastError().text().toStdString());
        }
        try {
            switch (v) {
            case 0:
                migrateV0ToV1(db);
                break;
            case 1:
                migrateV1ToV2(db);
                break;
            case 2:
                migrateV2ToV3(db);
                break;
            default:
                throw std::runtime_error(
                    QStringLiteral("no migration defined from v%1")
                        .arg(v)
                        .toStdString());
            }
            QSqlQuery bump(db);
            if (currentVersion(db) == 0) {
                // schema_version table didn't exist at start of this
                // migration; migrateV0ToV1 created it, insert the row.
                if (!bump.exec(QStringLiteral(
                        "INSERT INTO schema_version (version) VALUES (%1)")
                                   .arg(v + 1))) {
                    throw std::runtime_error(
                        bump.lastError().text().toStdString());
                }
            } else {
                if (!bump.exec(QStringLiteral(
                        "UPDATE schema_version SET version = %1")
                                   .arg(v + 1))) {
                    throw std::runtime_error(
                        bump.lastError().text().toStdString());
                }
            }
        } catch (...) {
            db.rollback();
            throw;
        }
        if (!db.commit()) {
            throw std::runtime_error(
                QStringLiteral("migration commit failed: ").toStdString() +
                db.lastError().text().toStdString());
        }
    }
}

void Migrations::migrateV0ToV1(QSqlDatabase& db) {
    QSqlQuery q(db);

    exec(q, QStringLiteral(
                "CREATE TABLE IF NOT EXISTS schema_version ("
                "    version INTEGER NOT NULL"
                ")"));

    exec(q, QStringLiteral(
                "CREATE TABLE projects ("
                "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "    parent_project_id INTEGER,"
                "    name TEXT NOT NULL,"
                "    root_path TEXT NOT NULL,"
                "    created INTEGER NOT NULL,"
                "    updated INTEGER NOT NULL,"
                "    last_used INTEGER NOT NULL,"
                "    FOREIGN KEY (parent_project_id) REFERENCES projects(id) "
                "        ON DELETE SET NULL"
                ")"));
    exec(q, QStringLiteral("CREATE INDEX idx_projects_name ON projects(name)"));
    exec(q, QStringLiteral(
                "CREATE INDEX idx_projects_parent ON projects(parent_project_id)"));
    exec(q, QStringLiteral(
                "CREATE INDEX idx_projects_last_used ON projects(last_used)"));

    exec(q, QStringLiteral(
                "CREATE TABLE sessions ("
                "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "    project_id INTEGER,"
                "    title TEXT NOT NULL,"
                "    cwd TEXT NOT NULL,"
                "    chat_draft TEXT,"
                "    created INTEGER NOT NULL,"
                "    updated INTEGER NOT NULL,"
                "    last_used INTEGER NOT NULL,"
                "    FOREIGN KEY (project_id) REFERENCES projects(id) "
                "        ON DELETE SET NULL,"
                "    CHECK (chat_draft IS NULL OR length(chat_draft) < 1048576)"
                ")"));
    exec(q, QStringLiteral(
                "CREATE INDEX idx_sessions_project_id ON sessions(project_id)"));
    exec(q, QStringLiteral(
                "CREATE INDEX idx_sessions_last_used ON sessions(last_used)"));

    exec(q, QStringLiteral(
                "CREATE TABLE buffers ("
                "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "    session_id INTEGER NOT NULL,"
                "    file_path TEXT NOT NULL,"
                "    dirty_content BLOB,"
                "    saved_mtime INTEGER,"
                "    opened INTEGER NOT NULL,"
                "    FOREIGN KEY (session_id) REFERENCES sessions(id) "
                "        ON DELETE CASCADE"
                ")"));
    exec(q, QStringLiteral(
                "CREATE INDEX idx_buffers_session_id ON buffers(session_id)"));
    exec(q, QStringLiteral(
                "CREATE UNIQUE INDEX idx_buffers_session_path "
                "ON buffers(session_id, file_path)"));

    exec(q, QStringLiteral(
                "CREATE TABLE messages ("
                "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "    session_id INTEGER NOT NULL,"
                "    msg_role TEXT NOT NULL,"
                "    content BLOB NOT NULL,"
                "    created INTEGER NOT NULL,"
                "    FOREIGN KEY (session_id) REFERENCES sessions(id) "
                "        ON DELETE CASCADE"
                ")"));
    exec(q, QStringLiteral(
                "CREATE INDEX idx_messages_session_id_created "
                "ON messages(session_id, created)"));

    // `attachment_scope` (not `scope`) because `SCOPE` is a
    // reserved word in SQL:2016 (appears in the SQL/XML extension).
    // The C++ struct field stays `AttachmentRow::scope` per the
    // application-layer carve-out in the project SQL rule — the
    // DAO translates between the C++ term and the column.
    exec(q, QStringLiteral(
                "CREATE TABLE attachments ("
                "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "    session_id INTEGER NOT NULL,"
                "    message_id INTEGER,"
                "    attachment_scope TEXT NOT NULL,"
                "    uuid TEXT NOT NULL UNIQUE,"
                "    file_path TEXT NOT NULL,"
                "    mime_type TEXT NOT NULL,"
                "    size_bytes INTEGER NOT NULL,"
                "    created INTEGER NOT NULL,"
                "    FOREIGN KEY (session_id) REFERENCES sessions(id) "
                "        ON DELETE CASCADE,"
                "    FOREIGN KEY (message_id) REFERENCES messages(id) "
                "        ON DELETE CASCADE,"
                "    CHECK (attachment_scope IN ('draft', 'message'))"
                ")"));
    exec(q, QStringLiteral(
                "CREATE INDEX idx_attachments_session_scope "
                "ON attachments(session_id, attachment_scope)"));
    exec(q, QStringLiteral(
                "CREATE INDEX idx_attachments_message_id "
                "ON attachments(message_id)"));

    // settings_kv: column names dodge SQL reserved words (§4.3 comment)
    exec(q, QStringLiteral(
                "CREATE TABLE settings_kv ("
                "    name       TEXT PRIMARY KEY,"
                "    data_value BLOB NOT NULL"
                ")"));
}

void Migrations::migrateV1ToV2(QSqlDatabase& db) {
    QSqlQuery q(db);
    exec(q, QStringLiteral(
                "ALTER TABLE buffers ADD COLUMN tab_position "
                "INTEGER NOT NULL DEFAULT 0"));
}

void Migrations::migrateV2ToV3(QSqlDatabase& db) {
    QSqlQuery q(db);
    exec(q, QStringLiteral("ALTER TABLE buffers RENAME TO buffers_old"));
    exec(q, QStringLiteral(
                "CREATE TABLE buffers ("
                "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "    session_id INTEGER NOT NULL,"
                "    file_path TEXT,"
                "    dirty_content BLOB,"
                "    saved_mtime INTEGER,"
                "    opened INTEGER NOT NULL,"
                "    tab_position INTEGER NOT NULL DEFAULT 0,"
                "    FOREIGN KEY (session_id) REFERENCES sessions(id) "
                "        ON DELETE CASCADE"
                ")"));
    exec(q, QStringLiteral(
                "INSERT INTO buffers "
                "SELECT id, session_id, file_path, dirty_content, "
                "saved_mtime, opened, tab_position FROM buffers_old"));
    exec(q, QStringLiteral("DROP TABLE buffers_old"));
    exec(q, QStringLiteral(
                "CREATE INDEX idx_buffers_session_id "
                "ON buffers(session_id)"));
    exec(q, QStringLiteral(
                "CREATE UNIQUE INDEX idx_buffers_session_path "
                "ON buffers(session_id, file_path)"));
}
