// SPDX-License-Identifier: GPL-3.0-only
//
// Phase 1 GUI stub per docs/phase-1.md §8.3. The real QMainWindow,
// chat view, and engine wiring arrive in Phases 2-4. This file exists
// so the top-level CMake graph has a real binary to produce and the
// packaging path (debian/claudeqt.install + dh_shlibdeps) can be
// validated end to end before we start writing the real application.
#include <QApplication>
#include <QLabel>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("ClaudeQt");
    QApplication::setOrganizationName("ClaudeQt");
    QApplication::setApplicationVersion(CLAUDEQT_VERSION);

    QLabel label(QStringLiteral("ClaudeQt %1 — Phase 1 scaffold")
                     .arg(QString::fromLatin1(CLAUDEQT_VERSION)));
    label.setAlignment(Qt::AlignCenter);
    label.setMargin(24);
    label.resize(420, 120);
    label.show();

    return QApplication::exec();
}
