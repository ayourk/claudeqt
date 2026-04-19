// SPDX-License-Identifier: GPL-3.0-only
//
// Schema migration framework. Each migration is a static function;
// runAll() reads the current schema_version row, iterates missing
// migrations in order, runs each inside a transaction, and bumps
// schema_version atomically.

#pragma once

#include <QSqlDatabase>

class Migrations {
public:
    static constexpr int kLatestVersion = 3;

    // Reads schema_version.version, or 0 if the table doesn't exist.
    static int currentVersion(QSqlDatabase& db);

    // Runs every migration from currentVersion() up to kLatestVersion.
    // Throws std::runtime_error on SQL failure (callers should let it
    // propagate — a broken DB means we can't boot).
    static void runAll(QSqlDatabase& db);

private:
    static void migrateV0ToV1(QSqlDatabase& db);
    static void migrateV1ToV2(QSqlDatabase& db);
    static void migrateV2ToV3(QSqlDatabase& db);
};
