// SPDX-License-Identifier: GPL-3.0-only
//
// GUI entry point. Constructs Persistence (DB + migrations),
// installs the Fusion QStyle, applies the Theme's palette + QSS
// stylesheet, then shows MainWindow.
//
// setApplicationName() is the single fork hook that drives
// everything downstream: QStandardPaths, the DB connection name,
// MIME type suffixes, and the window title. organizationName is
// deliberately unset so QStandardPaths::AppLocalDataLocation
// resolves to $XDG_DATA_HOME/<app>/ on Linux. If both are set, Qt
// nests <org>/<app>.
#include "instancehub.h"
#include "mainwindow.h"
#include "persistence.h"
#include "theme.h"

#include <QApplication>
#include <QIcon>
#include <QStyleFactory>

int main(int argc, char* argv[]) {
    // Force-link the Qt resource bundle from the static ui lib —
    // without this the linker drops qInitResources_resources and
    // :/icons/app.svg is unreachable at runtime.
    Q_INIT_RESOURCE(resources);

    QApplication app(argc, argv);
    QApplication::setApplicationName(QString::fromLatin1(APP_NAME));
    QApplication::setApplicationVersion(
        QString::fromLatin1(APP_VERSION));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/app.svg")));

    // Fusion is the only Qt style that honors palette + QSS
    // consistently across Linux desktops.
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    // Palette first, stylesheet second — QSS overrides the palette
    // for widgets that use QSS, and palette handles the ones that
    // don't (native file dialogs, some third-party widgets).
    QPalette p = QApplication::palette();
    Theme::instance().applyToPalette(p);
    QApplication::setPalette(p);
    qApp->setStyleSheet(Theme::instance().globalStyleSheet());

    // Open the DB, run migrations. Any failure here is fatal — we
    // can't usefully run without storage. The exception will unwind
    // and terminate the process with a C++ runtime message.
    (void) Persistence::instance();

    auto& hub = InstanceHub::instance();
    hub.start();
    Persistence::instance().connectToHub();

    MainWindow window;
    window.show();

    const int rc = QApplication::exec();
    hub.shutdown();
    return rc;
}
