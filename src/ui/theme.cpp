// SPDX-License-Identifier: GPL-3.0-only
#include "theme.h"

namespace {

QString rgba(const QColor& c) {
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(c.red())
        .arg(c.green())
        .arg(c.blue())
        .arg(c.alpha());
}

QString hex(const QColor& c) {
    return c.name(QColor::HexRgb);
}

} // namespace

Theme& Theme::instance() {
    static Theme s;
    return s;
}

void Theme::applyToPalette(QPalette& p) const {
    p.setColor(QPalette::Window,          background());
    p.setColor(QPalette::WindowText,      foreground());
    p.setColor(QPalette::Base,            inputBase());
    p.setColor(QPalette::AlternateBase,   dark());
    p.setColor(QPalette::Text,            foreground());
    p.setColor(QPalette::PlaceholderText, placeholderText());
    p.setColor(QPalette::Button,          buttonBase());
    p.setColor(QPalette::ButtonText,      foreground());
    p.setColor(QPalette::Highlight,       selectionBackground());
    p.setColor(QPalette::HighlightedText, selectionForeground());
    p.setColor(QPalette::Link,            link());
    p.setColor(QPalette::LinkVisited,     linkVisited());
    p.setColor(QPalette::ToolTipBase,     dark());
    p.setColor(QPalette::ToolTipText,     foreground());
    p.setColor(QPalette::Mid,             mid());
    p.setColor(QPalette::Dark,            dark());

    p.setColor(QPalette::Disabled, QPalette::WindowText,
               disabledForeground());
    p.setColor(QPalette::Disabled, QPalette::Text,
               disabledForeground());
    p.setColor(QPalette::Disabled, QPalette::ButtonText,
               disabledForeground());
    p.setColor(QPalette::Disabled, QPalette::Base,
               disabledBackground());
    p.setColor(QPalette::Disabled, QPalette::Button,
               disabledBackground());
}

QString Theme::globalStyleSheet() const {
    // Assembled as one big QString rather than resource-loaded so
    // Theme's color tokens can be interpolated without a preprocessor
    // step. QString::arg() was considered but the number of slots
    // (~40) makes positional args unreadable; direct string
    // concatenation with literal hex/rgba colors is clearer and
    // still a one-shot at startup.
    const QString bg     = hex(background());
    const QString fg     = hex(foreground());
    const QString input  = hex(inputBase());
    const QString btn    = hex(buttonBase());
    const QString border = hex(mid());
    const QString darkBg = hex(dark());

    const QString acc    = hex(accent());
    const QString accH   = hex(accentHover());
    const QString dest   = hex(destructive());
    const QString destH  = hex(destructiveHover());

    const QString btnH   = hex(buttonHover());
    const QString hover  = rgba(hoverTint());
    const QString focus  = hex(focusRing());
    const QString disFg  = hex(disabledForeground());
    const QString disBg  = hex(disabledBackground());

    const QString selBg  = rgba(selectionBackground());
    const QString selFg  = hex(selectionForeground());

    const QString err    = hex(error());
    const QString warn   = hex(warning());
    const QString ok     = hex(success());
    const QString inf    = hex(info());

    const QString sbTrk  = hex(scrollbarTrack());
    const QString sbThm  = hex(scrollbarThumb());
    const QString sbThH  = hex(scrollbarThumbHover());

    const QString banner = hex(chatBanner());

    QString qss;
    qss.reserve(4096);

    // Window chrome
    qss += QStringLiteral(
        "QMainWindow, QDialog, QWidget {"
        "  background-color: %1;"
        "  color: %2;"
        "}\n").arg(bg, fg);

    // Treeview
    qss += QStringLiteral(
        "QTreeView {"
        "  background-color: %1;"
        "  alternate-background-color: %2;"
        "  selection-background-color: %3;"
        "  selection-color: %4;"
        "  border: 1px solid %5;"
        "}\n"
        "QTreeView::item:hover { background-color: %6; }\n")
        .arg(bg, darkBg, selBg, fg, border, hover);

    // Inputs. `combobox-popup: 0` forces Qt to render the QComboBox
    // popup as a QListView rather than the native menu-style popup;
    // without it, applying QSS to QComboBox causes the popup to be
    // sized too short and render scroll indicators (up/down arrows)
    // even when every item could fit.
    qss += QStringLiteral(
        "QLineEdit, QPlainTextEdit, QTextEdit, QSpinBox, QComboBox {"
        "  background-color: %1;"
        "  color: %2;"
        "  border: 1px solid %3;"
        "  padding: 4px;"
        "  selection-background-color: %4;"
        "  selection-color: %5;"
        "}\n"
        "QComboBox { combobox-popup: 0; }\n"
        "QLineEdit:hover, QPlainTextEdit:hover, QTextEdit:hover,"
        "QSpinBox:hover, QComboBox:hover { border-color: %6; }\n"
        "QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus,"
        "QSpinBox:focus, QComboBox:focus {"
        "  border: 2px solid %7; padding: 3px;"
        "}\n"
        "QLineEdit:disabled, QPlainTextEdit:disabled, QTextEdit:disabled,"
        "QSpinBox:disabled, QComboBox:disabled {"
        "  background-color: %8; color: %9; border-color: %8;"
        "}\n")
        .arg(input, fg, border, selBg, selFg, acc, focus, disBg, disFg);

    // QComboBox dropdown popup. Without an explicit item rule the
    // view inherits the QComboBox frame's padding, which throws off
    // Qt's popup height calculation — the last row ends up clipped
    // (split through the middle). Pinning an explicit item height
    // plus zero popup padding makes the math close.
    qss += QStringLiteral(
        "QComboBox QAbstractItemView {"
        "  background-color: %1;"
        "  color: %2;"
        "  border: 1px solid %3;"
        "  selection-background-color: %4;"
        "  selection-color: %5;"
        "  outline: 0;"
        "  padding: 0;"
        "}\n"
        "QComboBox QAbstractItemView::item {"
        "  padding: 4px 8px;"
        "  min-height: 22px;"
        "}\n")
        .arg(input, fg, border, selBg, selFg);

    // Buttons (base)
    qss += QStringLiteral(
        "QPushButton {"
        "  background-color: %1;"
        "  color: %2;"
        "  border: 1px solid %3;"
        "  padding: 6px 12px;"
        "  border-radius: 3px;"
        "}\n"
        "QPushButton:hover {"
        "  background-color: %4;"
        "  border-color: %5;"
        "}\n"
        "QPushButton:focus {"
        "  outline: none;"
        "  border: 2px solid %6;"
        "  padding: 5px 11px;"
        "}\n"
        "QPushButton:pressed { background-color: %7; }\n"
        "QPushButton:disabled {"
        "  background-color: %8;"
        "  color: %9;"
        "  border-color: %8;"
        "}\n")
        .arg(btn, fg, border, btnH, acc, focus, darkBg, disBg, disFg);

    // Accent / destructive buttons by objectName
    qss += QStringLiteral(
        "QPushButton#accentButton {"
        "  background-color: %1;"
        "  color: %2;"
        "  border-color: %1;"
        "}\n"
        "QPushButton#accentButton:hover {"
        "  background-color: %3;"
        "  border-color: %3;"
        "}\n"
        "QPushButton#destructiveButton {"
        "  background-color: %4;"
        "  color: %5;"
        "  border-color: %4;"
        "}\n"
        "QPushButton#destructiveButton:hover {"
        "  background-color: %6;"
        "  border-color: %6;"
        "}\n")
        .arg(acc, bg, accH, dest, fg, destH);

    // Menus, toolbar items
    qss += QStringLiteral(
        "QMenu::item:selected, QMenuBar::item:selected {"
        "  background-color: %1;"
        "}\n"
        "QToolButton:hover {"
        "  background-color: %1;"
        "  border: 1px solid %2;"
        "  border-radius: 3px;"
        "}\n")
        .arg(hover, acc);

    // Splitter
    qss += QStringLiteral(
        "QSplitter::handle { background-color: %1; }\n"
        "QSplitter::handle:horizontal { width: 3px; }\n"
        "QSplitter::handle:vertical { height: 3px; }\n")
        .arg(border);

    // Status/menu/tool bars
    qss += QStringLiteral(
        "QStatusBar, QMenuBar, QToolBar {"
        "  background-color: %1;"
        "  color: %2;"
        "  border: none;"
        "}\n")
        .arg(darkBg, fg);

    // Chat banner
    qss += QStringLiteral(
        "QLabel#chatBanner {"
        "  background-color: %1;"
        "  color: %2;"
        "  padding: 8px;"
        "  border-bottom: 1px solid %3;"
        "}\n")
        .arg(banner, bg, border);

    // Rich-text anchor styling (CSS <a> analogue for QLabel with
    // Qt::RichText). QPalette::Link/LinkVisited handles most cases,
    // but this QSS hook lets individual QLabels override via
    // inherited cascade. textFormat="1" is Qt::RichText.
    qss += QStringLiteral(
        "QLabel[textFormat=\"1\"] a {"
        "  color: %1;"
        "  text-decoration: underline;"
        "}\n"
        "QLabel[textFormat=\"1\"] a:hover {"
        "  color: %2;"
        "}\n")
        .arg(hex(link()), hex(accentHover()));

    // Scrollbars
    qss += QStringLiteral(
        "QScrollBar:vertical, QScrollBar:horizontal {"
        "  background: %1;"
        "  border: none;"
        "}\n"
        "QScrollBar:vertical { width: 12px; }\n"
        "QScrollBar:horizontal { height: 12px; }\n"
        "QScrollBar::handle {"
        "  background: %2;"
        "  border-radius: 6px;"
        "  min-height: 20px;"
        "  min-width: 20px;"
        "}\n"
        "QScrollBar::handle:hover { background: %3; }\n"
        "QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }\n"
        "QScrollBar::add-page, QScrollBar::sub-page { background: none; }\n")
        .arg(sbTrk, sbThm, sbThH);

    // Semantic state labels
    qss += QStringLiteral(
        "QLabel#errorLabel   { color: %1; }\n"
        "QLabel#warningLabel { color: %2; }\n"
        "QLabel#successLabel { color: %3; }\n"
        "QLabel#infoLabel    { color: %4; }\n")
        .arg(err, warn, ok, inf);

    // Check/radio hover + focus
    qss += QStringLiteral(
        "QCheckBox:hover, QRadioButton:hover { color: %1; }\n"
        "QCheckBox:focus, QRadioButton:focus {"
        "  outline: 1px solid %2;"
        "  outline-offset: 2px;"
        "}\n")
        .arg(accH, focus);

    return qss;
}
