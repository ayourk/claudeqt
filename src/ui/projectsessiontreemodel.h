// SPDX-License-Identifier: GPL-3.0-only
//
// ProjectSessionTreeModel — QAbstractItemModel backing the left-
// pane tree view.
//
// Scope:
//   - Reads projects + sessions from Persistence on construction
//     and on every projectsChanged/sessionsChanged signal
//   - Two root-level item kinds: top-level projects (folder icon,
//     bold) and orphan sessions (document icon, plain)
//   - Projects nest recursively; sub-projects listed before
//     sessions inside any project node
//   - Sort order from settings_kv keys `tree.project_sort_order`
//     and `tree.session_sort_order`; defaults to `last_used DESC`
//     (§10.2 paragraph "Default ordering: most-recently-used
//     first, newest to oldest"). The alphabetical path is
//     alpha-by-name (projects) / alpha-by-title (sessions),
//     NOCASE.
//   - Drag-drop: sessions move between projects and to the
//     invisible root (orphan); projects reparent with a
//     canDropMimeData cycle guard. MIME types are derived from
//     QCoreApplication::applicationName() at call time so a fork
//     inherits a fresh namespace.
//
// Deferred:
//   - CRUD dialogs and context menu
//   - Inline rename via Qt::EditRole (model is read-only for now)
//   - Settings-driven live re-sort when the user flips project/
//     session sort order (model exposes reloadFromDb() which the
//     settings UI can call, but there's no wiring yet)

#pragma once

#include <QAbstractItemModel>
#include <QObject>
#include <QString>
#include <QVariant>
#include <QtGlobal>

#include <memory>
#include <vector>

class ProjectSessionTreeModel : public QAbstractItemModel {
    Q_OBJECT
public:
    enum ItemType { Project = 0, Session = 1 };

    enum Roles {
        ItemTypeRole = Qt::UserRole + 1,  // returns ItemType
        ItemIdRole = Qt::UserRole + 2,    // returns qint64 DB row id
        CwdRole = Qt::UserRole + 3,       // returns QString (sessions only)
        IsOrphanRole = Qt::UserRole + 4,  // returns bool
    };

    explicit ProjectSessionTreeModel(QObject* parent = nullptr);
    ~ProjectSessionTreeModel() override;

    // QAbstractItemModel contract
    QModelIndex index(int row, int column,
                      const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index,
                  int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex& index, const QVariant& value,
                 int role = Qt::EditRole) override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

    // Drag-drop
    Qt::DropActions supportedDropActions() const override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
    bool canDropMimeData(const QMimeData* data, Qt::DropAction action,
                         int row, int column,
                         const QModelIndex& parent) const override;
    bool dropMimeData(const QMimeData* data, Qt::DropAction action,
                      int row, int column,
                      const QModelIndex& parent) override;

    // Force a full reload from the DAO. Called from the
    // Persistence signal slots and also exposed so Settings can
    // trigger a re-sort without needing a private friend
    // declaration.
    void reloadFromDb();

    // Locate the model index for a given session row id. Returns
    // an invalid index if the session is not in the tree. Used by
    // MainWindow to restore `session.last_active_id` selection at
    // startup and to sync the tree with the chat shell's active
    // session.
    QModelIndex indexForSession(qint64 sessionId) const;

    // MIME type constants — evaluated at call time against
    // QCoreApplication::applicationName() per §10.3.
    static QString mimeSessionId();
    static QString mimeProjectId();

private slots:
    void onProjectsChanged();
    void onSessionsChanged();

private:
    struct TreeNode {
        ItemType type;
        qint64 dbId = 0;
        QString displayName;
        QString tooltip;         // cwd for sessions, root_path for projects
        bool isOrphan = false;   // sessions with NULL project_id
        TreeNode* parent = nullptr;
        std::vector<std::unique_ptr<TreeNode>> children;
    };

    TreeNode* nodeFromIndex(const QModelIndex& idx) const;
    int rowOfNode(const TreeNode* node) const;
    TreeNode* findSessionNode(TreeNode* under, qint64 sessionId) const;

    // Rebuild `m_root` in place. Called from reloadFromDb.
    void rebuildTree();

    std::unique_ptr<TreeNode> m_root;  // sentinel — children are the visible roots
};
