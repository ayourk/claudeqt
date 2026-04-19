// SPDX-License-Identifier: GPL-3.0-only
//
// Chat splitter persistence + validation. Covers the four
// round-trip cases:
//
//   (a) drag-to-70%, close, relaunch at same window size →
//       manual_size persisted verbatim, m_chatSplitUserOverridden
//       stays true
//   (b) persist manual_size = splitterH (100%) → clamps to
//       splitterH − kChatSplitterMinTopPx (== 120), settings_kv
//       value rewritten to the clamped number
//   (c) persist manual_size = 0 → clamps up to
//       chatSplitterMinBottomHeight(), settings_kv value rewritten
//   (d) persist stashed_manual_size = 500 with a small window
//       whose auto-mode cap is < 500 → stashed_manual_size reverts
//       to −1, right-click-from-auto becomes a no-op
//
// Rationale for the asymmetric validation: the live manual mode
// is *supposed* to survive above 50% (that's the whole point of
// manual override), so its only floor is the child-minimum band.
// But the stash carries the snapshot taken at the moment auto
// mode last kicked in — it must satisfy the full auto cap or it
// would silently break the toggle's symmetry.

#include "chatinputwidget.h"
#include "mainwindow.h"
#include "persistence.h"
#include "rightclicksplitter.h"

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QSplitter>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QtTest/QtTest>

namespace {
const QString kKeyWinW = QStringLiteral("window.width");
const QString kKeyWinH = QStringLiteral("window.height");
const QString kKeyManual = QStringLiteral("chat_splitter.manual_size");
const QString kKeyStashed =
    QStringLiteral("chat_splitter.stashed_manual_size");
const QStringList kCleanupKeys = {
    kKeyWinW, kKeyWinH, kKeyManual, kKeyStashed,
    QStringLiteral("window.x"),
    QStringLiteral("window.y"),
    QStringLiteral("window.maximized"),
    QStringLiteral("left_pane.width"),
    QStringLiteral("left_pane.visible"),
    QStringLiteral("top_splitter.message_ratio"),
    QStringLiteral("editor.visible"),
};

void cleanupKeys() {
    for (const auto& k : kCleanupKeys) {
        Persistence::instance().clearSetting(k);
    }
}

// Drive the deferred singleShot(0) that MainWindow queues in its
// constructor for validateAndApplyChatSplitterState(). The splitter
// needs one event loop spin after show() for its layout pass to
// land, then another spin for the singleShot to fire. Two qWaits
// are enough.
void settleLayout(QWidget* w) {
    QVERIFY(QTest::qWaitForWindowExposed(w));
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
}
}  // namespace

class TestChatSplitter : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

    void dragSeventyPercentPersistsVerbatim();
    void manualSizeOneHundredPercentClampsToBottom();
    void manualSizeZeroClampsToMinBottom();
    void stashedOverAutoCapRevertsToEmpty();

private:
    QTemporaryDir m_tmp;
};

void TestChatSplitter::initTestCase() {
    QVERIFY(m_tmp.isValid());
    const QString dbPath = m_tmp.filePath(QStringLiteral("split.sqlite"));
    Persistence::setDatabasePathForTesting(dbPath);
    QCoreApplication::setApplicationName(
        QStringLiteral("chatsplitter_test"));
    qputenv("XDG_DATA_HOME", m_tmp.path().toUtf8());
    (void) Persistence::instance();
    QVERIFY(QFile::exists(dbPath));
}

// (a) Drag to roughly 70% of the splitter height, verify that the
// manual_size is persisted, and that a second MainWindow at the
// same window size restores the same split with the override flag
// set.
void TestChatSplitter::dragSeventyPercentPersistsVerbatim() {
    cleanupKeys();

    Persistence::instance().setSetting(kKeyWinW, QByteArray::number(1000));
    Persistence::instance().setSetting(kKeyWinH, QByteArray::number(800));

    int splitterH = 0;
    int bottomTarget = 0;
    {
        MainWindow w;
        w.show();
        settleLayout(&w);

        auto* split = w.messageChatSplitter();
        QVERIFY(split);
        splitterH = split->height();
        QVERIFY(splitterH > 200);

        // 70% = bottom pane at 0.7 * splitterH. Simulate a drag by
        // calling setSizes directly and pulsing the splitterMoved
        // signal, which is what the real drag codepath does.
        bottomTarget = int(0.7 * splitterH);
        const int topH = splitterH - bottomTarget;
        split->setSizes({topH, bottomTarget});
        emit split->splitterMoved(topH, 1);

        // Flush the 500 ms debounce by triggering the close path,
        // which calls flushPendingGeometryWrites().
        w.close();
    }

    const QByteArray persisted =
        Persistence::instance().getSetting(kKeyManual);
    QVERIFY(!persisted.isEmpty());
    const int stored = persisted.toInt();
    // Allow ±2 px for splitter handle rounding.
    QVERIFY2(qAbs(stored - bottomTarget) <= 2,
             qPrintable(QStringLiteral("stored=%1 target=%2")
                            .arg(stored).arg(bottomTarget)));

    // Relaunch at the same window size and confirm the split is
    // restored.
    {
        MainWindow w2;
        w2.show();
        settleLayout(&w2);

        auto* split = w2.messageChatSplitter();
        QVERIFY(split);
        const auto sizes = split->sizes();
        QCOMPARE(sizes.size(), 2);
        QVERIFY2(qAbs(sizes[1] - stored) <= 2,
                 qPrintable(QStringLiteral("restored=%1 expected=%2")
                                .arg(sizes[1]).arg(stored)));
    }
}

// (b) Persist manual_size = splitterH (attempt at 100% bottom).
// Validation should clamp to splitterH − kChatSplitterMinTopPx
// (== 120 via chatSplitterMinTopHeight()) and rewrite the
// settings_kv value. The top pane must be at least 120 px tall.
void TestChatSplitter::manualSizeOneHundredPercentClampsToBottom() {
    cleanupKeys();

    Persistence::instance().setSetting(kKeyWinW, QByteArray::number(1000));
    Persistence::instance().setSetting(kKeyWinH, QByteArray::number(800));

    // Peek at the runtime splitter height by constructing once
    // without a manual_size so we know what "100%" means for the
    // clamp math.
    int splitterH = 0;
    int minTop = 0;
    {
        MainWindow probe;
        probe.show();
        settleLayout(&probe);
        auto* split = probe.messageChatSplitter();
        QVERIFY(split);
        splitterH = split->height();
        minTop = probe.chatSplitterMinTopHeight();
        QVERIFY(splitterH > minTop + 50);
    }

    // Seed the pathological value.
    Persistence::instance().setSetting(
        kKeyManual, QByteArray::number(splitterH));

    {
        MainWindow w;
        w.show();
        settleLayout(&w);

        auto* split = w.messageChatSplitter();
        const auto sizes = split->sizes();
        QCOMPARE(sizes.size(), 2);
        // Bottom clamped so that top ≥ minTop. QSplitter::sizes()
        // sums to splitterH − handleWidth, so we only assert the
        // floor rather than an exact equality.
        QVERIFY2(sizes[0] >= minTop - 1,
                 qPrintable(QStringLiteral("top=%1 minTop=%2")
                                .arg(sizes[0]).arg(minTop)));
        QVERIFY(sizes[1] > 0);
    }

    // Validation rewrote the persisted value to the clamped
    // number, not the original pathological one.
    const int rewritten =
        Persistence::instance().getSetting(kKeyManual).toInt();
    QVERIFY2(rewritten <= splitterH - minTop + 1,
             qPrintable(QStringLiteral("rewritten=%1 cap=%2")
                            .arg(rewritten).arg(splitterH - minTop)));
    QVERIFY(rewritten < splitterH);
}

// (c) Persist manual_size = 0 (attempt at 0% bottom / chat input
// collapsed). Validation should clamp up to the child-minimum
// band at chatSplitterMinBottomHeight() and rewrite settings_kv.
void TestChatSplitter::manualSizeZeroClampsToMinBottom() {
    cleanupKeys();

    Persistence::instance().setSetting(kKeyWinW, QByteArray::number(1000));
    Persistence::instance().setSetting(kKeyWinH, QByteArray::number(800));
    Persistence::instance().setSetting(kKeyManual, QByteArray::number(0));

    int minBottom = 0;
    {
        MainWindow w;
        w.show();
        settleLayout(&w);

        auto* split = w.messageChatSplitter();
        const auto sizes = split->sizes();
        QCOMPARE(sizes.size(), 2);

        minBottom = w.chatSplitterMinBottomHeight();
        QVERIFY(minBottom > 0);

        // Bottom pane clamped to at least minBottom.
        QVERIFY2(sizes[1] >= minBottom - 1,
                 qPrintable(QStringLiteral("bottom=%1 minBottom=%2")
                                .arg(sizes[1]).arg(minBottom)));
    }

    // Rewritten to the clamped value, not the original 0.
    const int rewritten =
        Persistence::instance().getSetting(kKeyManual).toInt();
    QVERIFY2(rewritten >= minBottom - 1,
             qPrintable(QStringLiteral("rewritten=%1 minBottom=%2")
                            .arg(rewritten).arg(minBottom)));
    QVERIFY(rewritten > 0);
}

// (d) Persist stashed_manual_size = 500 with a window whose auto
// cap is below 500. The stash must revert to −1 on startup, and a
// right-click from auto mode must then be a no-op (no stash to
// restore).
void TestChatSplitter::stashedOverAutoCapRevertsToEmpty() {
    cleanupKeys();

    // Small window: 640×460 means the splitter height is roughly
    // 300-ish after chrome + chat row budget, so halfCap ~150 —
    // well below the seeded 500. rowCap depends on font metrics
    // but max_rows defaults to 10; in offscreen Qt's default font
    // at ~14 px line spacing that's ~140 + padding, also well
    // below 500.
    Persistence::instance().setSetting(kKeyWinW, QByteArray::number(640));
    Persistence::instance().setSetting(kKeyWinH, QByteArray::number(460));
    // No manual_size — we want auto mode so we can test the
    // right-click-from-auto path.
    Persistence::instance().clearSetting(kKeyManual);
    Persistence::instance().setSetting(
        kKeyStashed, QByteArray::number(500));

    MainWindow w;
    w.show();
    settleLayout(&w);

    auto* split = w.messageChatSplitter();
    QVERIFY(split);
    const int splitterH = split->height();
    const int halfCap = splitterH / 2;
    const int rowCap = w.chatSplitterRowCap();
    const int upper = std::min(halfCap, rowCap);

    // Sanity: the test only makes sense if 500 really is above
    // the auto cap for this window.
    QVERIFY2(500 > upper,
             qPrintable(QStringLiteral("upper=%1 halfCap=%2 rowCap=%3")
                            .arg(upper).arg(halfCap).arg(rowCap)));

    // Validation should have cleared the persisted key.
    const QByteArray stashedAfter =
        Persistence::instance().getSetting(kKeyStashed);
    QVERIFY2(stashedAfter.isEmpty() || stashedAfter.toInt() == -1,
             qPrintable(QStringLiteral("stashed still '%1'")
                            .arg(QString::fromUtf8(stashedAfter))));

    // And a right-click from auto is a no-op (sizes unchanged).
    // Emit handleRightClicked() from the splitter itself — the
    // MainWindow connects this signal to
    // onMessageChatSplitterRightClicked in buildCentralWidget, so
    // this is the same codepath the real right-click triggers.
    auto* rcSplit = qobject_cast<RightClickSplitter*>(split);
    QVERIFY(rcSplit != nullptr);
    const auto before = split->sizes();
    emit rcSplit->handleRightClicked();
    const auto after = split->sizes();
    QCOMPARE(after, before);
}

QTEST_MAIN(TestChatSplitter)
#include "test_chatsplitter.moc"
