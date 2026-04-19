// SPDX-License-Identifier: GPL-3.0-only
#include "actionregistry.h"

#include <QAction>
#include <QDebug>

ActionRegistry& ActionRegistry::instance() {
    static ActionRegistry s;
    return s;
}

ActionRegistry::ActionRegistry() = default;

QAction* ActionRegistry::registerAction(const QString& id,
                                        const QString& text,
                                        const QKeySequence& defaultShortcut,
                                        const QString& statusTip) {
    if (id.isEmpty()) {
        qWarning() << "ActionRegistry::registerAction: empty id";
        return nullptr;
    }
    if (m_entries.contains(id)) {
        qWarning() << "ActionRegistry::registerAction: duplicate id" << id;
        return nullptr;
    }

    auto* act = new QAction(text, this);
    if (!defaultShortcut.isEmpty()) {
        act->setShortcut(defaultShortcut);
    }
    if (!statusTip.isEmpty()) {
        act->setStatusTip(statusTip);
    }

    m_entries.insert(id, Entry{act, defaultShortcut});
    m_order.append(id);

    emit actionRegistered(id);
    return act;
}

QAction* ActionRegistry::action(const QString& id) const {
    const auto it = m_entries.constFind(id);
    if (it == m_entries.constEnd()) {
        Q_ASSERT_X(false, "ActionRegistry::action",
                   qPrintable(QStringLiteral("unknown action id: %1").arg(id)));
        return nullptr;
    }
    return it->action;
}

bool ActionRegistry::contains(const QString& id) const {
    return m_entries.contains(id);
}

QList<QString> ActionRegistry::actionIds() const {
    return m_order;
}

QKeySequence ActionRegistry::defaultShortcut(const QString& id) const {
    const auto it = m_entries.constFind(id);
    if (it == m_entries.constEnd()) {
        return {};
    }
    return it->defaultShortcut;
}

void ActionRegistry::clearForTesting() {
    for (auto& entry : m_entries) {
        if (entry.action) {
            entry.action->deleteLater();
        }
    }
    m_entries.clear();
    m_order.clear();
}
