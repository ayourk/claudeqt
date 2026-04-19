// SPDX-License-Identifier: GPL-3.0-only
//
// Unit tests for ThemedQtDialog. Coverage:
//   - aboutToAccept signal fires on accept() and a veto prevents
//     QDialog::accept() from committing
//   - shake() runs without crashing and leaves the dialog at its
//     original position when the animation finishes
//   - setAccentButton and setDestructiveButton apply the
//     objectName selectors the QSS stylesheet hooks into
//
// This is a GUI test — QTEST_MAIN picks QApplication because the
// CMakeLists.txt links Qt6::Widgets, and the ctest `ENVIRONMENT`
// sets QT_QPA_PLATFORM=offscreen so no display is needed.

#include "theme.h"
#include "themedqtdialog.h"

#include <QDialogButtonBox>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QSignalSpy>
#include <QTimer>
#include <QtTest/QtTest>

class TestThemedQtDialog : public QObject {
    Q_OBJECT

private slots:
    void acceptEmitsAboutToAccept();
    void vetoPreventsAccept();
    void shakeRunsWithoutCrash();
    void accentButtonGetsObjectName();
    void destructiveButtonGetsObjectName();
    void themeStyleSheetIsNonEmpty();
};

void TestThemedQtDialog::acceptEmitsAboutToAccept() {
    ThemedQtDialog dlg;
    QSignalSpy spy(&dlg, &ThemedQtDialog::aboutToAccept);
    QMetaObject::invokeMethod(&dlg, "accept", Qt::DirectConnection);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(dlg.result(), int(QDialog::Accepted));
}

void TestThemedQtDialog::vetoPreventsAccept() {
    ThemedQtDialog dlg;
    QObject::connect(&dlg, &ThemedQtDialog::aboutToAccept, &dlg,
                     &ThemedQtDialog::vetoAccept);
    // Start with a sentinel: result() defaults to Rejected, so we
    // expect it to stay that way after the vetoed accept().
    QMetaObject::invokeMethod(&dlg, "accept", Qt::DirectConnection);
    QCOMPARE(dlg.result(), int(QDialog::Rejected));

    // Second accept() with the veto connection removed should
    // commit normally — proves the veto flag is not sticky.
    QObject::disconnect(&dlg, &ThemedQtDialog::aboutToAccept, &dlg,
                        &ThemedQtDialog::vetoAccept);
    QMetaObject::invokeMethod(&dlg, "accept", Qt::DirectConnection);
    QCOMPARE(dlg.result(), int(QDialog::Accepted));
}

void TestThemedQtDialog::shakeRunsWithoutCrash() {
    ThemedQtDialog dlg;
    dlg.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dlg));
    const QPoint origin = dlg.pos();

    dlg.shake();
    // 400 ms animation; wait a bit past that for it to finalize.
    QTest::qWait(500);

    // After the shake completes, the dialog should be back at its
    // starting position (both start and end keyframes are `origin`).
    QCOMPARE(dlg.pos(), origin);

    // Re-entrant shake must not crash or drift.
    dlg.shake();
    dlg.shake();
    QTest::qWait(500);
    QCOMPARE(dlg.pos(), origin);
}

void TestThemedQtDialog::accentButtonGetsObjectName() {
    ThemedQtDialog dlg;
    auto* btn = new QPushButton(&dlg);
    dlg.setAccentButton(btn);
    QCOMPARE(btn->objectName(), QStringLiteral("accentButton"));
    QVERIFY(btn->isDefault());
}

void TestThemedQtDialog::destructiveButtonGetsObjectName() {
    ThemedQtDialog dlg;
    auto* btn = new QPushButton(&dlg);
    dlg.setDestructiveButton(btn);
    QCOMPARE(btn->objectName(), QStringLiteral("destructiveButton"));
}

void TestThemedQtDialog::themeStyleSheetIsNonEmpty() {
    // Smoke check: Theme's QSS must contain both the accent button
    // selector and the destructive button selector — if either is
    // missing, setAccentButton / setDestructiveButton have no
    // visual effect and that's a silent regression worth catching.
    const QString qss = Theme::instance().globalStyleSheet();
    QVERIFY(!qss.isEmpty());
    QVERIFY(qss.contains(QStringLiteral("QPushButton#accentButton")));
    QVERIFY(qss.contains(QStringLiteral("QPushButton#destructiveButton")));
}

QTEST_MAIN(TestThemedQtDialog)
#include "test_themedqtdialog.moc"
