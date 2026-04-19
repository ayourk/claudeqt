// SPDX-License-Identifier: GPL-3.0-only
//
// SettingsDialog — tabbed modal dialog on top of ThemedQtDialog:
// General / Editor / About. Every writable control writes to
// `settings_kv` through Persistence, which emits
// `settingChanged(name)` so live-apply consumers (the editor
// pane, the tree model sort path) can react without a restart.
//
// Buttons: OK (write pending + close), Apply (write pending,
// keep open), Cancel (discard pending). Apply is disabled until
// at least one control has been edited.
//
// Dialog size is intentionally not pinned — QTabWidget's sizeHint
// drives the layout and later additions can grow tabs without
// squeezing into the current dialog's envelope.

#pragma once

#include "themedqtdialog.h"

#include <QHash>
#include <QString>
#include <QVariant>

class QCheckBox;
class QComboBox;
class QFontComboBox;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QWidget;

class SettingsDialog : public ThemedQtDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr);
    ~SettingsDialog() override;

    // Exposed for test hooks — pick tab by index (0=General,
    // 1=Editor, 2=About).
    void setCurrentTab(int index);

    // Programmatically trigger the Apply path without closing.
    // Returns true if all pending values validated and wrote.
    bool applyPending();

private slots:
    void onControlChanged();
    void onBrowseDefaultCwd();
    void onResetWindowLayout();

private:
    void buildGeneralTab();
    void buildEditorTab();
    void buildAboutTab();
    void loadCurrentValues();
    void markDirty();
    bool validatePending();  // shake/veto on invalid default cwd

    QTabWidget* m_tabs = nullptr;

    // General tab controls
    QComboBox* m_language = nullptr;
    QComboBox* m_themeVariant = nullptr;
    QLineEdit* m_defaultCwd = nullptr;
    QCheckBox* m_confirmSessionDelete = nullptr;
    QCheckBox* m_confirmProjectDelete = nullptr;
    QComboBox* m_projectSortOrder = nullptr;
    QComboBox* m_sessionSortOrder = nullptr;
    QSpinBox* m_activationDwellMs = nullptr;

    // Editor tab controls
    QFontComboBox* m_fontFamily = nullptr;
    QSpinBox* m_fontSize = nullptr;
    QSpinBox* m_tabWidth = nullptr;
    QCheckBox* m_insertSpaces = nullptr;
    QComboBox* m_lineWrapMode = nullptr;
    QCheckBox* m_lineNumbers = nullptr;
    QCheckBox* m_highlightCurrentLine = nullptr;
    QSpinBox* m_chatInputMaxRows = nullptr;
    QSpinBox* m_chatInputMinRows = nullptr;

    // Button bar
    QPushButton* m_okButton = nullptr;
    QPushButton* m_applyButton = nullptr;
    QPushButton* m_cancelButton = nullptr;

    bool m_dirty = false;
    // Re-entrancy guard so loadCurrentValues()'s programmatic
    // setters don't flip m_dirty.
    bool m_suppressDirty = false;
};
