// SPDX-License-Identifier: GPL-3.0-only
#include "themedqtdialog.h"

#include <QDialogButtonBox>
#include <QEasingCurve>
#include <QPoint>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QStyle>

ThemedQtDialog::ThemedQtDialog(QWidget* parent) : QDialog(parent) {
    // QDialog defaults are fine: modal by default when exec()'d,
    // Enter → accept(), Escape → reject(), window close → reject().
}

ThemedQtDialog::~ThemedQtDialog() = default;

void ThemedQtDialog::setAccentButton(QPushButton* btn) {
    if (!btn) return;
    btn->setObjectName(QStringLiteral("accentButton"));
    btn->setDefault(true);
    btn->style()->unpolish(btn);
    btn->style()->polish(btn);
}

void ThemedQtDialog::setDestructiveButton(QPushButton* btn) {
    if (!btn) return;
    btn->setObjectName(QStringLiteral("destructiveButton"));
    btn->style()->unpolish(btn);
    btn->style()->polish(btn);
}

void ThemedQtDialog::setHelpButton(QPushButton* btn) {
    if (!btn) return;
    btn->setObjectName(QStringLiteral("helpButton"));
    btn->style()->unpolish(btn);
    btn->style()->polish(btn);
}

QDialogButtonBox*
ThemedQtDialog::buildButtonBox(QDialogButtonBox::StandardButtons buttons) {
    auto* box = new QDialogButtonBox(buttons, this);
    connect(box, &QDialogButtonBox::accepted, this, &ThemedQtDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &ThemedQtDialog::reject);
    return box;
}

void ThemedQtDialog::shake() {
    if (!m_shakeAnim) {
        m_shakeAnim = new QPropertyAnimation(this, "pos", this);
        m_shakeAnim->setDuration(400);
        m_shakeAnim->setEasingCurve(QEasingCurve::InOutSine);
    }

    m_shakeAnim->stop();

    // Resolve the "origin" from the current animation's start value
    // if we're re-entering a running shake — otherwise the current
    // pos(). Guards against a double-shake that would drift the
    // dialog away from its original position.
    const QPoint origin = m_shakeAnim->startValue().isValid()
                              ? m_shakeAnim->startValue().toPoint()
                              : pos();

    m_shakeAnim->setStartValue(origin);
    m_shakeAnim->setEndValue(origin);
    m_shakeAnim->setKeyValueAt(0.000, origin);
    m_shakeAnim->setKeyValueAt(0.125, origin + QPoint(10, 0));
    m_shakeAnim->setKeyValueAt(0.250, origin - QPoint(10, 0));
    m_shakeAnim->setKeyValueAt(0.375, origin + QPoint(10, 0));
    m_shakeAnim->setKeyValueAt(0.500, origin - QPoint(10, 0));
    m_shakeAnim->setKeyValueAt(0.625, origin + QPoint(10, 0));
    m_shakeAnim->setKeyValueAt(0.750, origin - QPoint(10, 0));
    m_shakeAnim->setKeyValueAt(0.875, origin + QPoint(10, 0));
    m_shakeAnim->setKeyValueAt(1.000, origin);
    m_shakeAnim->start();
}

void ThemedQtDialog::vetoAccept() {
    m_acceptVetoed = true;
}

void ThemedQtDialog::accept() {
    m_acceptVetoed = false;
    emit aboutToAccept();
    if (m_acceptVetoed) {
        m_acceptVetoed = false;
        return;
    }
    QDialog::accept();
}
