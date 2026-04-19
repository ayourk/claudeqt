// SPDX-License-Identifier: GPL-3.0-only
//
// Minimal helper process for test_instancehub_ipc. Connects to an
// InstanceHub leader at a caller-supplied socket path, waits for
// events, writes what it received to a sentinel file, optionally
// publishes an event back, then exits.
//
// Usage: hub_child_helper <socket-path> <sentinel-path> [publish]
//   - Connects to the hub at <socket-path>
//   - Writes received event tags (hex) to <sentinel-path>, one per line
//   - If "publish" is passed, publishes a kSessionsChanged event
//     after receiving the first inbound event

#include "instancehub.h"

#include <QCoreApplication>
#include <QFile>
#include <QTextStream>
#include <QTimer>

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(
        QStringLiteral("hub_child_helper"));

    if (argc < 3) return 1;

    const QString socketPath = QString::fromUtf8(argv[1]);
    const QString sentinelPath = QString::fromUtf8(argv[2]);
    const bool shouldPublish = (argc >= 4 &&
        QByteArray(argv[3]) == QByteArrayLiteral("publish"));

    InstanceHub::setRelayJitterForTesting(50, 250);
    InstanceHub::setSocketPathForTesting(socketPath);
    auto& hub = InstanceHub::instance();

    QFile sentinel(sentinelPath);
    sentinel.open(QIODevice::WriteOnly | QIODevice::Truncate);
    QTextStream out(&sentinel);

    bool published = false;

    QObject::connect(&hub, &InstanceHub::eventReceived,
                     &app, [&](const InstanceHub::Event& e) {
        out << QString::number(static_cast<int>(e.tag), 16)
            << " " << e.seqno << "\n";
        out.flush();

        if (shouldPublish && !published) {
            published = true;
            InstanceHub::Event reply;
            reply.tag = InstanceHub::kSessionsChanged;
            hub.publish(reply);
        }
    });

    hub.start();

    QTimer::singleShot(5000, &app, [&]() {
        app.exit(2);
    });

    return app.exec();
}
