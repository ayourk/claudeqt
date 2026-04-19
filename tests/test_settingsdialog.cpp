// SPDX-License-Identifier: GPL-3.0-only
//
// Unit tests for SettingsDialog. Covers the non-modal
// surface area that can be exercised without blocking on
// ThemedQtDialog::exec():
//   - applyPending() writes every control's value to settings_kv
//   - Persistence emits settingChanged on each write
//   - EditorPaneWidget live-applies editor.font_size to open editors
//   - validatePending() rejects an invalid default cwd
//
// Modal OK/Cancel paths go through QDialog::exec() and are
// intentionally not exercised here.

#include "editorpanewidget.h"
#include "codeeditor.h"
#include "instancehub.h"
#include "persistence.h"
#include "settingsdialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QFile>
#include <QLineEdit>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest/QtTest>

class TestSettingsDialog : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init() { QTest::qWait(300); }
    void cleanup();

    void applyWritesAllKeys();
    void settingChangedEmittedOnWrite();
    void invalidDefaultCwdVetoes();
    void editorFontSizeLiveApplies();

private:
    QTemporaryDir m_tmp;
    void wipeSettings();
};

void TestSettingsDialog::initTestCase() {
    QVERIFY(m_tmp.isValid());
    const QString dbPath = m_tmp.filePath(QStringLiteral("settings.sqlite"));
    Persistence::setDatabasePathForTesting(dbPath);
    Persistence::setDeferralJitterForTesting(50, 250);
    QCoreApplication::setApplicationName(QStringLiteral("settings_test"));
    qputenv("XDG_DATA_HOME", m_tmp.path().toUtf8());

    const QString sockPath = m_tmp.filePath(QStringLiteral("hub.sock"));
    InstanceHub::setSocketPathForTesting(sockPath);
    InstanceHub::instance().start();

    (void) Persistence::instance();
    Persistence::instance().connectToHub();
    QVERIFY(QFile::exists(dbPath));
}

void TestSettingsDialog::cleanupTestCase() {
    InstanceHub::instance().shutdown();
    InstanceHub::setSocketPathForTesting(QString());
}

void TestSettingsDialog::cleanup() {
    wipeSettings();
}

void TestSettingsDialog::wipeSettings() {
    auto& db = Persistence::instance();
    const QStringList keys{
        QStringLiteral("ui.language"),
        QStringLiteral("ui.theme_variant"),
        QStringLiteral("ui.default_session_cwd"),
        QStringLiteral("ui.confirm_session_delete"),
        QStringLiteral("ui.confirm_project_delete"),
        QStringLiteral("tree.project_sort_order"),
        QStringLiteral("tree.session_sort_order"),
        QStringLiteral("editor.font_family"),
        QStringLiteral("editor.font_size"),
        QStringLiteral("editor.tab_width"),
        QStringLiteral("editor.insert_spaces"),
        QStringLiteral("editor.line_wrap_mode"),
        QStringLiteral("editor.line_numbers"),
        QStringLiteral("editor.highlight_current_line"),
        QStringLiteral("chat_input.max_rows"),
        QStringLiteral("chat_input.min_rows"),
    };
    for (const auto& k : keys) db.clearSetting(k);
}

// --- Apply path ---------------------------------------------------

void TestSettingsDialog::applyWritesAllKeys() {
    SettingsDialog dlg;

    // Mutate one control on each tab so m_dirty flips and
    // applyPending() actually walks the write path.
    auto* fontSize = dlg.findChild<QSpinBox*>(
        QStringLiteral("settingsFontSizeSpin"));
    QVERIFY(fontSize != nullptr);
    // Default cwd must exist for validatePending() to pass.
    auto* cwdEdit = dlg.findChild<QLineEdit*>(
        QStringLiteral("settingsDefaultCwdEdit"));
    QVERIFY(cwdEdit != nullptr);
    cwdEdit->setText(m_tmp.path());
    // Trigger the editor tab's dirty flag too.
    fontSize->setValue(15);

    QVERIFY(dlg.applyPending());

    auto& db = Persistence::instance();
    QCOMPARE(db.getSetting(QStringLiteral("editor.font_size")),
             QByteArrayLiteral("15"));
    QCOMPARE(db.getSetting(QStringLiteral("ui.default_session_cwd")),
             m_tmp.path().toUtf8());
}

void TestSettingsDialog::settingChangedEmittedOnWrite() {
    QSignalSpy spy(&Persistence::instance(),
                   &Persistence::settingChanged);
    Persistence::instance().setSetting(
        QStringLiteral("editor.font_size"), QByteArrayLiteral("12"));
    QTRY_VERIFY(spy.count() >= 1);
    const QList<QVariant> args = spy.last();
    QCOMPARE(args.at(0).toString(),
             QStringLiteral("editor.font_size"));
}

void TestSettingsDialog::invalidDefaultCwdVetoes() {
    SettingsDialog dlg;
    auto* cwdEdit = dlg.findChild<QLineEdit*>(
        QStringLiteral("settingsDefaultCwdEdit"));
    QVERIFY(cwdEdit != nullptr);
    cwdEdit->setText(QStringLiteral("/definitely/not/a/real/dir"));
    // Need at least one control dirty so applyPending actually
    // validates (early return if not dirty).
    auto* fontSize = dlg.findChild<QSpinBox*>(
        QStringLiteral("settingsFontSizeSpin"));
    QVERIFY(fontSize != nullptr);
    fontSize->setValue(17);
    QVERIFY(!dlg.applyPending());

    // The invalid cwd must NOT have been persisted. font_size
    // would also have been blocked because applyPending is
    // all-or-nothing — check it too.
    QVERIFY(Persistence::instance()
                .getSetting(QStringLiteral("ui.default_session_cwd"))
                .isEmpty());
    QVERIFY(Persistence::instance()
                .getSetting(QStringLiteral("editor.font_size"))
                .isEmpty());
}

void TestSettingsDialog::editorFontSizeLiveApplies() {
    // Open a real editor via EditorPaneWidget, then write
    // editor.font_size directly through Persistence. The pane
    // subscribes to settingChanged and should update the live
    // QFont on the open editor.
    const QString scratch = m_tmp.filePath(QStringLiteral("live.txt"));
    {
        QFile f(scratch);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QTextStream ts(&f);
        ts << "hello\n";
    }

    EditorPaneWidget pane;
    auto* editor = pane.openFile(scratch);
    QVERIFY(editor != nullptr);

    Persistence::instance().setSetting(
        QStringLiteral("editor.font_size"), QByteArrayLiteral("19"));

    QTRY_COMPARE(editor->font().pointSize(), 19);
}

QTEST_MAIN(TestSettingsDialog)
#include "test_settingsdialog.moc"
