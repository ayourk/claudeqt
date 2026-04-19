// SPDX-License-Identifier: GPL-3.0-only
#include "rightclicksplitter.h"

#include <QMouseEvent>

RightClickSplitter::RightClickSplitter(Qt::Orientation orientation,
                                       QWidget* parent)
    : QSplitter(orientation, parent) {}

QSplitterHandle* RightClickSplitter::createHandle() {
    return new RightClickSplitterHandle(orientation(), this);
}

RightClickSplitterHandle::RightClickSplitterHandle(Qt::Orientation orientation,
                                                   QSplitter* parent)
    : QSplitterHandle(orientation, parent) {}

void RightClickSplitterHandle::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::RightButton) {
        // Stock QSplitterHandle eats right clicks silently. We
        // forward them as a signal on the owning splitter so the
        // caller (typically MainWindow) can react — reset to
        // default, toggle auto/manual, etc. Swallowing the event
        // here means the base class never sees it and no drag
        // state is affected.
        if (auto* rc =
                qobject_cast<RightClickSplitter*>(splitter())) {
            emit rc->handleRightClicked();
        }
        event->accept();
        return;
    }
    QSplitterHandle::mousePressEvent(event);
}
