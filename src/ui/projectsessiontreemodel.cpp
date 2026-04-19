// SPDX-License-Identifier: GPL-3.0-only

#include "projectsessiontreemodel.h"

#include "persistence.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFont>
#include <QFontMetrics>
#include <QHash>
#include <QList>
#include <QMimeData>
#include <QSize>
#include <QStringList>

#include <algorithm>
#include <functional>

namespace {

// Settings keys for sort order. Defaults to "last_used" if the
// key is missing or holds an unrecognized value.
const QString kKeyProjectSortOrder =
    QStringLiteral("tree.project_sort_order");
const QString kKeySessionSortOrder =
    QStringLiteral("tree.session_sort_order");

enum class SortMode { LastUsed, Alpha, None };

SortMode readSortMode(const QString& key) {
    const QByteArray raw =
        Persistence::instance().getSetting(key);
    if (raw.isEmpty()) return SortMode::LastUsed;
    const QString s = QString::fromUtf8(raw).trimmed().toLower();
    if (s == QStringLiteral("name") || s == QStringLiteral("title") ||
        s == QStringLiteral("alpha")) {
        return SortMode::Alpha;
    }
    if (s == QStringLiteral("none") || s == QStringLiteral("off")) {
        return SortMode::None;
    }
    return SortMode::LastUsed;
}

}  // namespace

// --- MIME type constants ----------------------------------------

QString ProjectSessionTreeModel::mimeSessionId() {
    return QStringLiteral("application/x-%1-session-id")
        .arg(QCoreApplication::applicationName());
}

QString ProjectSessionTreeModel::mimeProjectId() {
    return QStringLiteral("application/x-%1-project-id")
        .arg(QCoreApplication::applicationName());
}

// --- Construction -----------------------------------------------

ProjectSessionTreeModel::ProjectSessionTreeModel(QObject* parent)
    : QAbstractItemModel(parent),
      m_root(std::make_unique<TreeNode>()) {
    connect(&Persistence::instance(), &Persistence::projectsChanged,
            this, &ProjectSessionTreeModel::onProjectsChanged);
    connect(&Persistence::instance(), &Persistence::sessionsChanged,
            this, &ProjectSessionTreeModel::onSessionsChanged);
    // Live re-sort when either sort-order key is written
    // through Settings. The full model-reset cycle is fine because
    // the tree structure is unchanged — only sibling ordering —
    // and the view-side modelReset handler on MainWindow restores
    // expansion state from tree.expanded_ids.
    connect(&Persistence::instance(), &Persistence::settingChanged,
            this, [this](const QString& name) {
                if (name == kKeyProjectSortOrder ||
                    name == kKeySessionSortOrder) {
                    reloadFromDb();
                }
            });
    rebuildTree();
}

ProjectSessionTreeModel::~ProjectSessionTreeModel() = default;

// --- Tree rebuild -----------------------------------------------

void ProjectSessionTreeModel::reloadFromDb() {
    beginResetModel();
    rebuildTree();
    endResetModel();
}

void ProjectSessionTreeModel::onProjectsChanged() { reloadFromDb(); }
void ProjectSessionTreeModel::onSessionsChanged() { reloadFromDb(); }

void ProjectSessionTreeModel::rebuildTree() {
    auto& db = Persistence::instance();

    // Fresh sentinel root; previous children are freed with the
    // old root.
    m_root = std::make_unique<TreeNode>();

    const auto allProjects = db.listProjects();
    const auto allSessions = db.listSessions();

    const SortMode projectSort = readSortMode(kKeyProjectSortOrder);
    const SortMode sessionSort = readSortMode(kKeySessionSortOrder);

    // Group projects by parent_project_id for fast lookup when
    // wiring the recursive tree. Orphans (nullopt parent) become
    // top-level project nodes.
    struct ProjectBucket {
        QList<ProjectRow> rows;
    };
    QHash<qint64, ProjectBucket> childrenOfProject;  // keyed by parent id
    QList<ProjectRow> topLevelProjects;
    for (const auto& p : allProjects) {
        if (p.parentProjectId.has_value()) {
            childrenOfProject[*p.parentProjectId].rows.append(p);
        } else {
            topLevelProjects.append(p);
        }
    }

    // Group sessions by project_id. Orphans (nullopt) become
    // top-level session nodes alongside top-level projects.
    QHash<qint64, QList<SessionRow>> sessionsByProject;
    QList<SessionRow> orphanSessions;
    for (const auto& s : allSessions) {
        if (s.projectId.has_value()) {
            sessionsByProject[*s.projectId].append(s);
        } else {
            orphanSessions.append(s);
        }
    }

    auto sortProjects = [projectSort](QList<ProjectRow>& rows) {
        if (projectSort == SortMode::None) return;
        std::sort(rows.begin(), rows.end(),
                  [projectSort](const ProjectRow& a, const ProjectRow& b) {
                      if (projectSort == SortMode::Alpha) {
                          const int cmp = QString::compare(
                              a.name, b.name, Qt::CaseInsensitive);
                          if (cmp != 0) return cmp < 0;
                          return a.id < b.id;
                      }
                      if (a.lastUsed != b.lastUsed)
                          return a.lastUsed > b.lastUsed;
                      return a.id > b.id;
                  });
    };

    auto sortSessions = [sessionSort](QList<SessionRow>& rows) {
        if (sessionSort == SortMode::None) return;
        std::sort(rows.begin(), rows.end(),
                  [sessionSort](const SessionRow& a, const SessionRow& b) {
                      if (sessionSort == SortMode::Alpha) {
                          const int cmp = QString::compare(
                              a.title, b.title, Qt::CaseInsensitive);
                          if (cmp != 0) return cmp < 0;
                          return a.id < b.id;
                      }
                      if (a.lastUsed != b.lastUsed)
                          return a.lastUsed > b.lastUsed;
                      return a.id > b.id;
                  });
    };

    // Recursive builder for a project subtree. `parentNode` is
    // the project node under construction; the function appends
    // sub-project children first, then session children.
    std::function<void(TreeNode*, qint64)> buildProject =
        [&](TreeNode* parentNode, qint64 pid) {
            QList<ProjectRow> subs = childrenOfProject.value(pid).rows;
            sortProjects(subs);
            for (const auto& sub : subs) {
                auto child = std::make_unique<TreeNode>();
                child->type = Project;
                child->dbId = sub.id;
                child->displayName = sub.name;
                child->tooltip = sub.rootPath;
                child->parent = parentNode;
                TreeNode* raw = child.get();
                parentNode->children.push_back(std::move(child));
                buildProject(raw, sub.id);
            }

            QList<SessionRow> sessions = sessionsByProject.value(pid);
            sortSessions(sessions);
            for (const auto& s : sessions) {
                auto child = std::make_unique<TreeNode>();
                child->type = Session;
                child->dbId = s.id;
                child->displayName = s.title;
                child->tooltip = s.cwd;
                child->isOrphan = false;
                child->parent = parentNode;
                parentNode->children.push_back(std::move(child));
            }
        };

    // Root level: top-level projects first, then orphan sessions.
    sortProjects(topLevelProjects);
    for (const auto& p : topLevelProjects) {
        auto node = std::make_unique<TreeNode>();
        node->type = Project;
        node->dbId = p.id;
        node->displayName = p.name;
        node->tooltip = p.rootPath;
        node->parent = m_root.get();
        TreeNode* raw = node.get();
        m_root->children.push_back(std::move(node));
        buildProject(raw, p.id);
    }

    sortSessions(orphanSessions);
    for (const auto& s : orphanSessions) {
        auto node = std::make_unique<TreeNode>();
        node->type = Session;
        node->dbId = s.id;
        node->displayName = s.title;
        node->tooltip = s.cwd;
        node->isOrphan = true;
        node->parent = m_root.get();
        m_root->children.push_back(std::move(node));
    }
}

// --- Index / parent traversal -----------------------------------

ProjectSessionTreeModel::TreeNode*
ProjectSessionTreeModel::nodeFromIndex(const QModelIndex& idx) const {
    if (!idx.isValid()) return m_root.get();
    return static_cast<TreeNode*>(idx.internalPointer());
}

int ProjectSessionTreeModel::rowOfNode(const TreeNode* node) const {
    if (!node || !node->parent) return 0;
    const auto& siblings = node->parent->children;
    for (int i = 0; i < int(siblings.size()); ++i) {
        if (siblings[i].get() == node) return i;
    }
    return 0;
}

QModelIndex ProjectSessionTreeModel::index(int row, int column,
                                           const QModelIndex& parent) const {
    if (column != 0) return {};
    TreeNode* parentNode = nodeFromIndex(parent);
    if (!parentNode) return {};
    if (row < 0 || row >= int(parentNode->children.size())) return {};
    TreeNode* child = parentNode->children[row].get();
    return createIndex(row, column, child);
}

QModelIndex ProjectSessionTreeModel::parent(const QModelIndex& child) const {
    if (!child.isValid()) return {};
    TreeNode* childNode = static_cast<TreeNode*>(child.internalPointer());
    if (!childNode || !childNode->parent || childNode->parent == m_root.get()) {
        return {};
    }
    TreeNode* parentNode = childNode->parent;
    return createIndex(rowOfNode(parentNode), 0, parentNode);
}

int ProjectSessionTreeModel::rowCount(const QModelIndex& parent) const {
    if (parent.column() > 0) return 0;
    TreeNode* node = nodeFromIndex(parent);
    return node ? int(node->children.size()) : 0;
}

int ProjectSessionTreeModel::columnCount(const QModelIndex&) const {
    return 1;
}

// --- Data --------------------------------------------------------

QVariant ProjectSessionTreeModel::data(const QModelIndex& index,
                                       int role) const {
    if (!index.isValid()) return {};
    TreeNode* node = static_cast<TreeNode*>(index.internalPointer());
    if (!node) return {};

    switch (role) {
    case Qt::DisplayRole:
    case Qt::EditRole:
        return node->displayName;
    case Qt::ToolTipRole:
        return node->tooltip;
    case Qt::FontRole:
        if (node->type == Project) {
            QFont f;
            f.setBold(true);
            return f;
        }
        return {};
    case ItemTypeRole:
        return int(node->type);
    case ItemIdRole:
        return QVariant::fromValue<qint64>(node->dbId);
    case CwdRole:
        return node->type == Session ? node->tooltip : QString();
    case IsOrphanRole:
        return node->isOrphan;
    case Qt::SizeHintRole: {
        QFontMetrics fm(QFont{});
        const int h = fm.height() + 10;
        return QSize(-1, h);
    }
    default:
        return {};
    }
}

Qt::ItemFlags ProjectSessionTreeModel::flags(const QModelIndex& index) const {
    Qt::ItemFlags base = QAbstractItemModel::flags(index);
    if (!index.isValid()) {
        // The invisible root is a valid drop target (for demoting
        // sessions to orphans and promoting projects to top-level).
        return base | Qt::ItemIsDropEnabled;
    }
    // Every item is selectable, enabled, draggable, a drop target,
    // and editable so double-click or F2 renames the
    // underlying row via setData(Qt::EditRole).
    return base | Qt::ItemIsSelectable | Qt::ItemIsEnabled |
           Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled |
           Qt::ItemIsEditable;
}

bool ProjectSessionTreeModel::setData(const QModelIndex& index,
                                      const QVariant& value, int role) {
    if (!index.isValid()) return false;
    if (role != Qt::EditRole) return false;

    TreeNode* node = static_cast<TreeNode*>(index.internalPointer());
    if (!node) return false;

    const QString newName = value.toString().trimmed();
    if (newName.isEmpty()) return false;
    if (newName == node->displayName) return false;

    // Snapshot the id + type before any DAO call. The rename
    // emits projectsChanged/sessionsChanged synchronously, which
    // routes into reloadFromDb → rebuildTree and frees every
    // TreeNode we're pointing at — touching `node` or `index`
    // after the DAO would be use-after-free.
    const ItemType kind = node->type;
    const qint64 dbId = node->dbId;

    try {
        if (kind == Project) {
            Persistence::instance().renameProject(dbId, newName);
        } else {
            Persistence::instance().renameSession(dbId, newName);
        }
    } catch (const std::exception&) {
        return false;
    }

    // The DAO's model reset has already notified views; no need
    // to emit dataChanged here and no way to safely reference the
    // now-dangling `index`.
    return true;
}

// --- Drag-drop ---------------------------------------------------

Qt::DropActions ProjectSessionTreeModel::supportedDropActions() const {
    return Qt::MoveAction;
}

QStringList ProjectSessionTreeModel::mimeTypes() const {
    return {mimeSessionId(), mimeProjectId()};
}

QMimeData* ProjectSessionTreeModel::mimeData(
    const QModelIndexList& indexes) const {
    if (indexes.isEmpty()) return nullptr;
    const QModelIndex& idx = indexes.first();
    if (!idx.isValid()) return nullptr;
    TreeNode* node = static_cast<TreeNode*>(idx.internalPointer());
    if (!node) return nullptr;

    auto* md = new QMimeData();
    const QByteArray payload =
        QByteArray::number(static_cast<qlonglong>(node->dbId));
    if (node->type == Session) {
        md->setData(mimeSessionId(), payload);
    } else {
        md->setData(mimeProjectId(), payload);
    }
    return md;
}

bool ProjectSessionTreeModel::canDropMimeData(
    const QMimeData* data, Qt::DropAction action, int /*row*/,
    int /*column*/, const QModelIndex& parent) const {
    if (action != Qt::MoveAction) return false;
    if (!data) return false;

    TreeNode* target = nodeFromIndex(parent);
    const bool targetIsRoot = (target == m_root.get());

    // Session drag: drop target must be a project node or the
    // invisible root (orphan).
    if (data->hasFormat(mimeSessionId())) {
        if (targetIsRoot) return true;
        if (!target) return false;
        return target->type == Project;
    }

    // Project drag: drop target is either the root (→ top-level)
    // or another project. Reject self-drops and cycle-creating
    // moves by walking the target's ancestor chain.
    if (data->hasFormat(mimeProjectId())) {
        const qint64 draggedId =
            data->data(mimeProjectId()).toLongLong();
        if (targetIsRoot) return true;
        if (!target) return false;
        if (target->type != Project) return false;
        if (target->dbId == draggedId) return false;  // self

        // Walk ancestors of target looking for draggedId.
        const TreeNode* walker = target;
        while (walker && walker != m_root.get()) {
            if (walker->dbId == draggedId) return false;
            walker = walker->parent;
        }
        return true;
    }

    return false;
}

bool ProjectSessionTreeModel::dropMimeData(const QMimeData* data,
                                           Qt::DropAction action, int row,
                                           int column,
                                           const QModelIndex& parent) {
    if (!canDropMimeData(data, action, row, column, parent)) {
        return false;
    }
    TreeNode* target = nodeFromIndex(parent);
    const bool targetIsRoot = (target == m_root.get());

    if (data->hasFormat(mimeSessionId())) {
        const qint64 sid = data->data(mimeSessionId()).toLongLong();
        std::optional<qint64> newProject;
        if (!targetIsRoot && target && target->type == Project) {
            newProject = target->dbId;
        }
        Persistence::instance().moveSessionToProject(sid, newProject);
        return true;
    }
    if (data->hasFormat(mimeProjectId())) {
        const qint64 pid = data->data(mimeProjectId()).toLongLong();
        std::optional<qint64> newParent;
        if (!targetIsRoot && target && target->type == Project) {
            newParent = target->dbId;
        }
        try {
            Persistence::instance().reparentProject(pid, newParent);
        } catch (const std::exception&) {
            return false;
        }
        return true;
    }
    return false;
}

// --- Session lookup ---------------------------------------------

ProjectSessionTreeModel::TreeNode*
ProjectSessionTreeModel::findSessionNode(TreeNode* under,
                                         qint64 sessionId) const {
    if (!under) return nullptr;
    for (const auto& child : under->children) {
        if (child->type == Session && child->dbId == sessionId) {
            return child.get();
        }
        if (child->type == Project) {
            if (auto* hit = findSessionNode(child.get(), sessionId)) {
                return hit;
            }
        }
    }
    return nullptr;
}

QModelIndex ProjectSessionTreeModel::indexForSession(
    qint64 sessionId) const {
    TreeNode* node = findSessionNode(m_root.get(), sessionId);
    if (!node) return {};
    return createIndex(rowOfNode(node), 0, node);
}
