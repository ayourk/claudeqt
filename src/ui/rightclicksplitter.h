// SPDX-License-Identifier: GPL-3.0-only
//
// RightClickSplitter — a QSplitter subclass whose handle emits a
// `handleRightClicked()` signal on right-mouse press. Stock Qt
// does not surface right-click on the handle, so we override
// `createHandle()` to return a custom `RightClickSplitterHandle`
// that intercepts `mousePressEvent`, forwards left/middle clicks
// to the base class (preserving drag behavior), and emits the
// signal for right clicks.
//
// Used by MainWindow in three places:
//   - outer horizontal splitter (right-click handle → reset
//     `left_pane.width` to 220)
//   - top message/editor splitter (right-click handle → reset
//     `top_splitter.message_ratio` to 0.5)
//   - messageChatSplitter (right-click
//     handle → auto/manual toggle with stash semantics)
//
// The same class is used for all three sites; each call site
// connects `handleRightClicked()` to a different slot. The
// splitter identifies which handle was clicked only by virtue of
// "this is the only right-click-listening handle on this
// splitter" — forks that want per-handle granularity can extend
// by passing the handle index as a signal parameter.

#pragma once

#include <QSplitter>
#include <QSplitterHandle>

class RightClickSplitter : public QSplitter {
    Q_OBJECT
public:
    explicit RightClickSplitter(Qt::Orientation orientation,
                                QWidget* parent = nullptr);

signals:
    // Emitted when the user right-clicks any handle on this
    // splitter. Slot consumers decide what to do with it (reset
    // to default, toggle mode, etc.) per the spec for their
    // specific call site.
    void handleRightClicked();

protected:
    QSplitterHandle* createHandle() override;
};

class RightClickSplitterHandle : public QSplitterHandle {
    Q_OBJECT
public:
    RightClickSplitterHandle(Qt::Orientation orientation,
                             QSplitter* parent);

protected:
    void mousePressEvent(QMouseEvent* event) override;
};
