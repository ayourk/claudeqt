// SPDX-License-Identifier: GPL-3.0-only
//
// Theme palette expansion. Verifies that Theme::applyToPalette
// routes every spec-listed QPalette role (§9.2.1) and that
// Theme::globalStyleSheet() contains the selector surface area the
// spec enumerates in §9.2. This is a pure render/serialization
// contract test — no widgets are created.

#include "theme.h"

#include <QPalette>
#include <QString>
#include <QtTest>

class TestTheme : public QObject {
    Q_OBJECT
private slots:
    void applyToPaletteRoutesActiveRoles();
    void applyToPaletteRoutesDisabledGroup();
    void globalStyleSheetContainsRequiredSelectors();
    void globalStyleSheetEmbedsThemeColors();
};

void TestTheme::applyToPaletteRoutesActiveRoles() {
    const Theme& t = Theme::instance();
    QPalette p;
    t.applyToPalette(p);

    QCOMPARE(p.color(QPalette::Window), t.background());
    QCOMPARE(p.color(QPalette::WindowText), t.foreground());
    QCOMPARE(p.color(QPalette::Base), t.inputBase());
    QCOMPARE(p.color(QPalette::AlternateBase), t.dark());
    QCOMPARE(p.color(QPalette::Text), t.foreground());
    QCOMPARE(p.color(QPalette::PlaceholderText), t.placeholderText());
    QCOMPARE(p.color(QPalette::Button), t.buttonBase());
    QCOMPARE(p.color(QPalette::ButtonText), t.foreground());
    QCOMPARE(p.color(QPalette::Highlight), t.selectionBackground());
    QCOMPARE(p.color(QPalette::HighlightedText), t.selectionForeground());
    QCOMPARE(p.color(QPalette::Link), t.link());
    QCOMPARE(p.color(QPalette::LinkVisited), t.linkVisited());
    QCOMPARE(p.color(QPalette::ToolTipBase), t.dark());
    QCOMPARE(p.color(QPalette::ToolTipText), t.foreground());
    QCOMPARE(p.color(QPalette::Mid), t.mid());
    QCOMPARE(p.color(QPalette::Dark), t.dark());
}

void TestTheme::applyToPaletteRoutesDisabledGroup() {
    const Theme& t = Theme::instance();
    QPalette p;
    t.applyToPalette(p);

    QCOMPARE(p.color(QPalette::Disabled, QPalette::WindowText),
             t.disabledForeground());
    QCOMPARE(p.color(QPalette::Disabled, QPalette::Text),
             t.disabledForeground());
    QCOMPARE(p.color(QPalette::Disabled, QPalette::ButtonText),
             t.disabledForeground());
    QCOMPARE(p.color(QPalette::Disabled, QPalette::Base),
             t.disabledBackground());
    QCOMPARE(p.color(QPalette::Disabled, QPalette::Button),
             t.disabledBackground());
}

void TestTheme::globalStyleSheetContainsRequiredSelectors() {
    const QString qss = Theme::instance().globalStyleSheet();

    // §9.2 QSS surface area — every selector listed in the spec
    // must be present. Missing one means a widget class falls back
    // to Fusion defaults and leaks out of the theme.
    const QStringList required = {
        QStringLiteral("QMainWindow"),
        QStringLiteral("QTreeView"),
        QStringLiteral("QTreeView::item:hover"),
        QStringLiteral("QLineEdit"),
        QStringLiteral("QPlainTextEdit"),
        QStringLiteral(":hover"),
        QStringLiteral(":focus"),
        QStringLiteral(":disabled"),
        QStringLiteral("QPushButton"),
        QStringLiteral("QPushButton:pressed"),
        QStringLiteral("QPushButton#accentButton"),
        QStringLiteral("QPushButton#destructiveButton"),
        QStringLiteral("QMenu::item:selected"),
        QStringLiteral("QMenuBar::item:selected"),
        QStringLiteral("QToolButton:hover"),
        QStringLiteral("QSplitter::handle"),
        QStringLiteral("QStatusBar"),
        QStringLiteral("QLabel#chatBanner"),
        QStringLiteral("QLabel[textFormat=\"1\"] a"),
        QStringLiteral("QScrollBar:vertical"),
        QStringLiteral("QScrollBar::handle"),
        QStringLiteral("QScrollBar::handle:hover"),
        QStringLiteral("QLabel#errorLabel"),
        QStringLiteral("QLabel#warningLabel"),
        QStringLiteral("QLabel#successLabel"),
        QStringLiteral("QLabel#infoLabel"),
        QStringLiteral("QCheckBox:hover"),
        QStringLiteral("QRadioButton:hover"),
        QStringLiteral("QCheckBox:focus"),
        QStringLiteral("QRadioButton:focus"),
        QStringLiteral("selection-background-color"),
    };

    for (const QString& token : required) {
        QVERIFY2(qss.contains(token),
                 qPrintable(QStringLiteral(
                     "globalStyleSheet() missing selector: %1").arg(token)));
    }
}

void TestTheme::globalStyleSheetEmbedsThemeColors() {
    const Theme& t = Theme::instance();
    const QString qss = t.globalStyleSheet();

    // A few of the palette tokens must appear verbatim in the
    // compiled QSS. Spot-checking the families catches interpolation
    // regressions (e.g. a refactor that forgot to substitute one
    // accessor). These are the colors most likely to drift because
    // they're used in multiple selectors.
    const QStringList mustContain = {
        t.background().name(QColor::HexRgb),
        t.foreground().name(QColor::HexRgb),
        t.accent().name(QColor::HexRgb),
        t.destructive().name(QColor::HexRgb),
        t.focusRing().name(QColor::HexRgb),
        t.error().name(QColor::HexRgb),
        t.warning().name(QColor::HexRgb),
        t.success().name(QColor::HexRgb),
        t.info().name(QColor::HexRgb),
        t.scrollbarThumb().name(QColor::HexRgb),
        t.scrollbarThumbHover().name(QColor::HexRgb),
        t.chatBanner().name(QColor::HexRgb),
        t.link().name(QColor::HexRgb),
    };

    for (const QString& hex : mustContain) {
        QVERIFY2(qss.contains(hex, Qt::CaseInsensitive),
                 qPrintable(QStringLiteral(
                     "globalStyleSheet() missing color: %1").arg(hex)));
    }
}

QTEST_MAIN(TestTheme)
#include "test_theme.moc"
