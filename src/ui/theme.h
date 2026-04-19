// SPDX-License-Identifier: GPL-3.0-only
//
// Theme singleton. Hardcoded dark palette today (single theme
// variant). A future release will parameterize this via
// settings_kv and emit colorsChanged so listeners can regenerate
// their stylesheets.
//
// All color tokens are surfaced as QColor accessors so widgets
// that draw themselves (custom delegates, QPainter code) can read
// them without parsing QSS. Widgets that style themselves via QSS
// use the compiled globalStyleSheet() string, which is built on
// demand and cached.

#pragma once

#include <QColor>
#include <QPalette>
#include <QString>

class Theme {
public:
    static Theme& instance();

    // --- Base surfaces ---
    QColor background() const { return QColor(QStringLiteral("#1e1e1e")); }
    QColor foreground() const { return QColor(QStringLiteral("#e0e0e0")); }
    QColor inputBase() const { return QColor(QStringLiteral("#2a2a2a")); }
    QColor buttonBase() const { return QColor(QStringLiteral("#353535")); }
    QColor mid() const { return QColor(QStringLiteral("#4a4a4a")); }
    QColor dark() const { return QColor(QStringLiteral("#1a1a1a")); }

    // --- Accent + destructive families ---
    QColor accent() const { return QColor(QStringLiteral("#3daee9")); }
    QColor accentHover() const { return QColor(QStringLiteral("#5ec5f0")); }
    QColor destructive() const { return QColor(QStringLiteral("#da4453")); }
    QColor destructiveHover() const { return QColor(QStringLiteral("#e86075")); }

    // --- Interactive states ---
    QColor buttonHover() const { return QColor(QStringLiteral("#454545")); }
    QColor hoverTint() const { return QColor(255, 255, 255, 20); }    // ~8% alpha
    QColor focusRing() const { return QColor(QStringLiteral("#3daee9")); }
    QColor pressedTint() const { return QColor(0, 0, 0, 38); }        // ~15% alpha
    QColor disabledForeground() const { return QColor(QStringLiteral("#6a6a6a")); }
    QColor disabledBackground() const { return QColor(QStringLiteral("#262626")); }
    QColor placeholderText() const { return QColor(QStringLiteral("#7a7a7a")); }

    // --- Selection ---
    QColor selectionBackground() const { return QColor(61, 174, 233, 102); } // 40% alpha
    QColor selectionForeground() const { return QColor(QStringLiteral("#ffffff")); }
    QColor highlight() const { return selectionBackground(); }

    // --- Rich text / links ---
    QColor link() const { return QColor(QStringLiteral("#5ec5f0")); }
    QColor linkVisited() const { return QColor(QStringLiteral("#a882d9")); }

    // --- Semantic state colors ---
    QColor error() const { return QColor(QStringLiteral("#e63946")); }
    QColor warning() const { return QColor(QStringLiteral("#f4a261")); }
    QColor success() const { return QColor(QStringLiteral("#68d391")); }
    QColor info() const { return QColor(QStringLiteral("#7cc4f5")); }

    // --- Scrollbars ---
    QColor scrollbarTrack() const { return QColor(QStringLiteral("#202020")); }
    QColor scrollbarThumb() const { return QColor(QStringLiteral("#4a4a4a")); }
    QColor scrollbarThumbHover() const { return QColor(QStringLiteral("#5a5a5a")); }

    // --- App-specific ---
    QColor chatBanner() const { return QColor(QStringLiteral("#f4a261")); }

    // Apply the theme's color tokens to a QPalette. Call in main()
    // right after QApplication construction.
    void applyToPalette(QPalette& p) const;

    // Compiled QSS for qApp->setStyleSheet(). Lazily generated;
    // deterministic — same call returns the same string.
    QString globalStyleSheet() const;

private:
    Theme() = default;
    Theme(const Theme&) = delete;
    Theme& operator=(const Theme&) = delete;
};
