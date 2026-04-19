// SPDX-License-Identifier: GPL-3.0-only
//
// ThemedQtDialog — the only dialog base class in this app. Every
// modal dialog subclasses this instead of QDialog so that:
//   1. Styling is consistent (QSS objectName selectors for
//      accentButton / destructiveButton roles)
//   2. Validation is consistent (aboutToAccept signal lets a slot
//      call vetoAccept() to cancel close; pair with shake() for UX)
//   3. Shake animation is a one-liner (~400 ms horizontal oscillation)
//
// The only exceptions to "always use ThemedQtDialog" are QFileDialog
// and QColorDialog, which stock Qt provides.

#pragma once

#include <QDialog>
#include <QDialogButtonBox>

class QPropertyAnimation;
class QPushButton;

class ThemedQtDialog : public QDialog {
    Q_OBJECT
public:
    explicit ThemedQtDialog(QWidget* parent = nullptr);
    ~ThemedQtDialog() override;

    // Tag a button as "primary / accent" — QSS #accentButton
    // selectors style it with the accent color. No ownership
    // transfer; caller retains the button.
    void setAccentButton(QPushButton* btn);

    // Tag a button as "destructive" — QSS #destructiveButton
    // selectors style it red.
    void setDestructiveButton(QPushButton* btn);

    // Tag a button as "help" — currently just applies the
    // objectName so QSS hooks are available; role-based
    // placement is planned for a future release.
    void setHelpButton(QPushButton* btn);

    // Horizontal shake, ~400 ms, 10 px amplitude, InOutSine easing.
    // Safe to call while already shaking — reuses the animation.
    void shake();

    // Convenience: QDialogButtonBox with standard buttons and the
    // ThemedQtDialog default connections wired up (accepted →
    // accept(), rejected → reject()). Caller adds the returned
    // widget to its own layout.
    QDialogButtonBox* buildButtonBox(QDialogButtonBox::StandardButtons buttons);

signals:
    // Emitted before QDialog::accept() commits. Any slot can call
    // vetoAccept() during handling to cancel this accept cycle —
    // typical validation-failure pattern:
    //     connect(this, &ThemedQtDialog::aboutToAccept, this, [this]{
    //         if (!valid()) { vetoAccept(); shake(); }
    //     });
    void aboutToAccept();

public slots:
    // Cancels the current accept cycle. Only meaningful from a slot
    // connected to aboutToAccept.
    void vetoAccept();

protected:
    void accept() override;

private:
    bool m_acceptVetoed = false;
    QPropertyAnimation* m_shakeAnim = nullptr;
};
