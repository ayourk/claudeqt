// SPDX-License-Identifier: GPL-3.0-only

#include "settingsdialog.h"

#include "editorpanewidget.h"
#include "mainwindow.h"
#include "persistence.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSpinBox>
#include <QSvgRenderer>
#include <QSysInfo>
#include <QTabWidget>
#include <QVBoxLayout>

#include <sqlite3.h>

namespace {

// --- Key helpers --------------------------------------------------

QString keyLanguage() { return QStringLiteral("ui.language"); }
QString keyThemeVariant() { return QStringLiteral("ui.theme_variant"); }
QString keyDefaultCwd() { return QStringLiteral("ui.default_session_cwd"); }
QString keyConfirmSessionDelete() {
    return QStringLiteral("ui.confirm_session_delete");
}
QString keyConfirmProjectDelete() {
    return QStringLiteral("ui.confirm_project_delete");
}
QString keyProjectSortOrder() {
    return QStringLiteral("tree.project_sort_order");
}
QString keySessionSortOrder() {
    return QStringLiteral("tree.session_sort_order");
}

QString keyFontFamily() { return QStringLiteral("editor.font_family"); }
QString keyFontSize() { return QStringLiteral("editor.font_size"); }
QString keyTabWidth() { return QStringLiteral("editor.tab_width"); }
QString keyInsertSpaces() { return QStringLiteral("editor.insert_spaces"); }
QString keyLineWrapMode() { return QStringLiteral("editor.line_wrap_mode"); }
QString keyLineNumbers() { return QStringLiteral("editor.line_numbers"); }
QString keyHighlightCurrentLine() {
    return QStringLiteral("editor.highlight_current_line");
}
QString keyChatInputMaxRows() {
    return QStringLiteral("chat_input.max_rows");
}
QString keyChatInputMinRows() {
    return QStringLiteral("chat_input.min_rows");
}
QString keyActivationDwellMs() {
    return QStringLiteral("session.activation_dwell_ms");
}

QString getStr(const QString& name, const QString& fallback) {
    const QByteArray raw = Persistence::instance().getSetting(name);
    if (raw.isEmpty()) return fallback;
    return QString::fromUtf8(raw);
}

int getInt(const QString& name, int fallback) {
    const QByteArray raw = Persistence::instance().getSetting(name);
    if (raw.isEmpty()) return fallback;
    bool ok = false;
    const int v = QString::fromUtf8(raw).toInt(&ok);
    return ok ? v : fallback;
}

bool getBool(const QString& name, bool fallback) {
    const QByteArray raw = Persistence::instance().getSetting(name);
    if (raw.isEmpty()) return fallback;
    const QString s = QString::fromUtf8(raw).trimmed().toLower();
    if (s == QStringLiteral("1") || s == QStringLiteral("true") ||
        s == QStringLiteral("yes")) {
        return true;
    }
    if (s == QStringLiteral("0") || s == QStringLiteral("false") ||
        s == QStringLiteral("no")) {
        return false;
    }
    return fallback;
}

void putStr(const QString& name, const QString& value) {
    Persistence::instance().setSetting(name, value.toUtf8());
}

void putInt(const QString& name, int value) {
    Persistence::instance().setSetting(
        name, QByteArray::number(value));
}

void putBool(const QString& name, bool value) {
    Persistence::instance().setSetting(
        name, value ? QByteArrayLiteral("1") : QByteArrayLiteral("0"));
}

QLabel* valueLabel(const QString& text) {
    auto* label = new QLabel(text);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(true);
    return label;
}

}  // namespace

// --- Construction -------------------------------------------------

SettingsDialog::SettingsDialog(QWidget* parent)
    : ThemedQtDialog(parent) {
    setWindowTitle(tr("Settings"));
    setObjectName(QStringLiteral("SettingsDialog"));

    auto* layout = new QVBoxLayout(this);
    m_tabs = new QTabWidget(this);
    layout->addWidget(m_tabs, /*stretch=*/1);

    buildGeneralTab();
    buildEditorTab();
    buildAboutTab();

    auto* buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Apply |
            QDialogButtonBox::Cancel,
        this);
    m_okButton = buttonBox->button(QDialogButtonBox::Ok);
    m_applyButton = buttonBox->button(QDialogButtonBox::Apply);
    m_cancelButton = buttonBox->button(QDialogButtonBox::Cancel);
    if (m_okButton) setAccentButton(m_okButton);
    if (m_applyButton) m_applyButton->setEnabled(false);

    connect(buttonBox, &QDialogButtonBox::accepted,
            this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected,
            this, &QDialog::reject);
    if (m_applyButton) {
        connect(m_applyButton, &QPushButton::clicked,
                this, [this]() { (void) applyPending(); });
    }

    connect(this, &ThemedQtDialog::aboutToAccept, this, [this]() {
        if (!validatePending()) {
            vetoAccept();
            shake();
            return;
        }
        // OK path: write then accept.
        (void) applyPending();
    });

    layout->addWidget(buttonBox);

    loadCurrentValues();
}

SettingsDialog::~SettingsDialog() = default;

void SettingsDialog::setCurrentTab(int index) {
    if (m_tabs) m_tabs->setCurrentIndex(index);
}

// --- General tab --------------------------------------------------

void SettingsDialog::buildGeneralTab() {
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);

    m_language = new QComboBox(page);
    m_language->addItem(tr("English"), QStringLiteral("en"));
    m_language->addItem(tr("中文 (Chinese)"), QStringLiteral("zh"));
    form->addRow(tr("Language:"), m_language);

    m_themeVariant = new QComboBox(page);
    m_themeVariant->addItem(tr("Dark"), QStringLiteral("dark"));
    m_themeVariant->addItem(tr("Light"), QStringLiteral("light"));
    m_themeVariant->addItem(tr("System"), QStringLiteral("system"));
    form->addRow(tr("Theme variant:"), m_themeVariant);

    auto* cwdRow = new QWidget(page);
    auto* cwdLayout = new QHBoxLayout(cwdRow);
    cwdLayout->setContentsMargins(0, 0, 0, 0);
    m_defaultCwd = new QLineEdit(cwdRow);
    m_defaultCwd->setObjectName(QStringLiteral("settingsDefaultCwdEdit"));
    auto* browseBtn = new QPushButton(tr("Browse…"), cwdRow);
    cwdLayout->addWidget(m_defaultCwd, /*stretch=*/1);
    cwdLayout->addWidget(browseBtn);
    form->addRow(tr("Default new-session cwd:"), cwdRow);
    connect(browseBtn, &QPushButton::clicked,
            this, &SettingsDialog::onBrowseDefaultCwd);

    m_confirmSessionDelete = new QCheckBox(tr("Confirm on session delete"),
                                           page);
    form->addRow(QString(), m_confirmSessionDelete);
    m_confirmProjectDelete = new QCheckBox(tr("Confirm on project delete"),
                                           page);
    form->addRow(QString(), m_confirmProjectDelete);

    m_projectSortOrder = new QComboBox(page);
    m_projectSortOrder->addItem(tr("Last used"), QStringLiteral("last_used"));
    m_projectSortOrder->addItem(tr("Name"), QStringLiteral("name"));
    m_projectSortOrder->addItem(tr("Off"), QStringLiteral("none"));
    form->addRow(tr("Project sort order:"), m_projectSortOrder);

    m_sessionSortOrder = new QComboBox(page);
    m_sessionSortOrder->addItem(tr("Last used"), QStringLiteral("last_used"));
    m_sessionSortOrder->addItem(tr("Title"), QStringLiteral("title"));
    m_sessionSortOrder->addItem(tr("Off"), QStringLiteral("none"));
    form->addRow(tr("Session sort order:"), m_sessionSortOrder);

    m_activationDwellMs = new QSpinBox(page);
    m_activationDwellMs->setRange(0, 30000);
    m_activationDwellMs->setSingleStep(500);
    m_activationDwellMs->setSuffix(tr(" ms"));
    m_activationDwellMs->setSpecialValueText(tr("Immediate"));
    form->addRow(tr("Activation dwell time:"), m_activationDwellMs);

    auto* resetBtn = new QPushButton(tr("Reset Window Layout…"), page);
    connect(resetBtn, &QPushButton::clicked,
            this, &SettingsDialog::onResetWindowLayout);
    form->addRow(QString(), resetBtn);

    connect(m_language, &QComboBox::currentIndexChanged,
            this, &SettingsDialog::onControlChanged);
    connect(m_themeVariant, &QComboBox::currentIndexChanged,
            this, &SettingsDialog::onControlChanged);
    connect(m_defaultCwd, &QLineEdit::textEdited,
            this, &SettingsDialog::onControlChanged);
    connect(m_confirmSessionDelete, &QCheckBox::toggled,
            this, &SettingsDialog::onControlChanged);
    connect(m_confirmProjectDelete, &QCheckBox::toggled,
            this, &SettingsDialog::onControlChanged);
    connect(m_projectSortOrder, &QComboBox::currentIndexChanged,
            this, &SettingsDialog::onControlChanged);
    connect(m_sessionSortOrder, &QComboBox::currentIndexChanged,
            this, &SettingsDialog::onControlChanged);
    connect(m_activationDwellMs, &QSpinBox::valueChanged,
            this, &SettingsDialog::onControlChanged);

    m_tabs->addTab(page, tr("General"));
}

// --- Editor tab ---------------------------------------------------

void SettingsDialog::buildEditorTab() {
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);

    m_fontFamily = new QFontComboBox(page);
    m_fontFamily->setFontFilters(QFontComboBox::MonospacedFonts);
    form->addRow(tr("Font family:"), m_fontFamily);

    m_fontSize = new QSpinBox(page);
    m_fontSize->setObjectName(QStringLiteral("settingsFontSizeSpin"));
    m_fontSize->setRange(8, 32);
    form->addRow(tr("Font size:"), m_fontSize);

    m_tabWidth = new QSpinBox(page);
    m_tabWidth->setRange(1, 8);
    form->addRow(tr("Tab width:"), m_tabWidth);

    m_insertSpaces = new QCheckBox(tr("Insert spaces for tab"), page);
    form->addRow(QString(), m_insertSpaces);

    m_lineWrapMode = new QComboBox(page);
    m_lineWrapMode->addItem(tr("No wrap"), QStringLiteral("nowrap"));
    m_lineWrapMode->addItem(tr("Soft wrap at viewport"),
                            QStringLiteral("soft"));
    form->addRow(tr("Line wrap mode:"), m_lineWrapMode);

    m_lineNumbers = new QCheckBox(tr("Show line numbers"), page);
    form->addRow(QString(), m_lineNumbers);

    m_highlightCurrentLine =
        new QCheckBox(tr("Highlight current line"), page);
    form->addRow(QString(), m_highlightCurrentLine);

    m_chatInputMaxRows = new QSpinBox(page);
    m_chatInputMaxRows->setRange(0, 20);
    m_chatInputMaxRows->setSpecialValueText(tr("No limit"));
    form->addRow(tr("Chat input max rows:"), m_chatInputMaxRows);

    m_chatInputMinRows = new QSpinBox(page);
    m_chatInputMinRows->setRange(1, 5);
    form->addRow(tr("Chat input min rows:"), m_chatInputMinRows);

    connect(m_fontFamily, &QFontComboBox::currentFontChanged,
            this, &SettingsDialog::onControlChanged);
    connect(m_fontSize, &QSpinBox::valueChanged,
            this, &SettingsDialog::onControlChanged);
    connect(m_tabWidth, &QSpinBox::valueChanged,
            this, &SettingsDialog::onControlChanged);
    connect(m_insertSpaces, &QCheckBox::toggled,
            this, &SettingsDialog::onControlChanged);
    connect(m_lineWrapMode, &QComboBox::currentIndexChanged,
            this, &SettingsDialog::onControlChanged);
    connect(m_lineNumbers, &QCheckBox::toggled,
            this, &SettingsDialog::onControlChanged);
    connect(m_highlightCurrentLine, &QCheckBox::toggled,
            this, &SettingsDialog::onControlChanged);
    connect(m_chatInputMaxRows, &QSpinBox::valueChanged,
            this, &SettingsDialog::onControlChanged);
    connect(m_chatInputMinRows, &QSpinBox::valueChanged,
            this, &SettingsDialog::onControlChanged);
    connect(m_chatInputMinRows, &QSpinBox::valueChanged,
            this, [this](int minVal) {
                const int maxVal = m_chatInputMaxRows->value();
                if (maxVal > 0 && maxVal < minVal)
                    m_chatInputMaxRows->setValue(minVal);
            });
    connect(m_chatInputMaxRows, &QSpinBox::valueChanged,
            this, [this, prevMax = std::make_shared<int>(0)](int maxVal) mutable {
                const int minVal = m_chatInputMinRows->value();
                if (maxVal > 0 && maxVal < minVal) {
                    const int target = (*prevMax >= minVal) ? 0 : minVal;
                    *prevMax = target;
                    m_chatInputMaxRows->setValue(target);
                    return;
                }
                *prevMax = maxVal;
            });

    m_tabs->addTab(page, tr("Editor"));
}

// --- About tab ----------------------------------------------------

void SettingsDialog::buildAboutTab() {
    // Force-link the resource init symbol when hosted from a test
    // binary that doesn't call Q_INIT_RESOURCE in its own main().
    Q_INIT_RESOURCE(resources);

    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    const QString appName = QCoreApplication::applicationName();
    const QString appVersion = QCoreApplication::applicationVersion();

    auto* titleLabel = new QLabel(
        QStringLiteral("<h2>%1 %2</h2>").arg(appName, appVersion), page);
    titleLabel->setAlignment(Qt::AlignCenter);
    titleLabel->setTextFormat(Qt::RichText);
    layout->addWidget(titleLabel);

    auto* logoLabel = new QLabel(page);
    logoLabel->setAlignment(Qt::AlignCenter);
    QSvgRenderer svgRenderer(QStringLiteral(":/icons/app.svg"));
    if (svgRenderer.isValid()) {
        QPixmap logoPix(96, 96);
        logoPix.fill(Qt::transparent);
        QPainter painter(&logoPix);
        svgRenderer.render(&painter);
        painter.end();
        logoLabel->setPixmap(logoPix);
    }
    layout->addWidget(logoLabel);

    auto* descLabel = new QLabel(
        QString::fromLatin1(APP_DESCRIPTION)
            + QStringLiteral("\n\nLicense: GPL 3.0 (only)\n")
            + QString::fromLatin1(APP_HOMEPAGE),
        page);
    descLabel->setAlignment(Qt::AlignCenter);
    descLabel->setWordWrap(true);
    descLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(descLabel);

    auto* depGroup = new QGroupBox(tr("Dependencies and Platform"), page);
    auto* depForm = new QFormLayout(depGroup);

    depForm->addRow(
        tr("Qt:"),
        valueLabel(QStringLiteral("%1 (runtime %2)")
                       .arg(QStringLiteral(QT_VERSION_STR),
                            QString::fromLatin1(qVersion()))));

#ifdef APP_KSH_VERSION_STRING
    depForm->addRow(tr("KSyntaxHighlighting:"),
                    valueLabel(QStringLiteral(APP_KSH_VERSION_STRING)));
#else
    depForm->addRow(tr("KSyntaxHighlighting:"),
                    valueLabel(tr("(build-time unknown)")));
#endif

#ifdef APP_CMARK_GFM_VERSION_STRING
    depForm->addRow(tr("cmark-gfm:"),
                    valueLabel(QStringLiteral(APP_CMARK_GFM_VERSION_STRING)));
#else
    depForm->addRow(tr("cmark-gfm:"),
                    valueLabel(tr("(build-time unknown)")));
#endif

    depForm->addRow(tr("SQLite:"),
                    valueLabel(QString::fromLatin1(sqlite3_libversion())));

#ifdef APP_CMAKE_VERSION_STRING
    depForm->addRow(tr("CMake:"),
                    valueLabel(QStringLiteral(APP_CMAKE_VERSION_STRING)));
#endif

#if defined(__clang__)
    depForm->addRow(
        tr("Compiler:"),
        valueLabel(QStringLiteral("Clang %1.%2.%3")
                       .arg(__clang_major__)
                       .arg(__clang_minor__)
                       .arg(__clang_patchlevel__)));
#elif defined(__GNUC__)
    depForm->addRow(
        tr("Compiler:"),
        valueLabel(QStringLiteral("GCC %1.%2.%3")
                       .arg(__GNUC__)
                       .arg(__GNUC_MINOR__)
                       .arg(__GNUC_PATCHLEVEL__)));
#endif

#ifdef APP_BUILD_TYPE_STRING
    depForm->addRow(tr("Build type:"),
                    valueLabel(QStringLiteral(APP_BUILD_TYPE_STRING)));
#endif

    depForm->addRow(
        tr("OS:"),
        valueLabel(QStringLiteral("%1 (%2)")
                       .arg(QSysInfo::prettyProductName(),
                            QSysInfo::currentCpuArchitecture())));

    depForm->addRow(
        tr("Kernel:"),
        valueLabel(QStringLiteral("%1 %2")
                       .arg(QSysInfo::kernelType(),
                            QSysInfo::kernelVersion())));

    depForm->addRow(tr("Hostname:"),
                    valueLabel(QSysInfo::machineHostName()));

    layout->addWidget(depGroup);

    auto* distGroup = new QGroupBox(tr("Distribution"), page);
    auto* distForm = new QFormLayout(distGroup);
#ifdef APP_PPA
    if(QFile::exists(QStringLiteral("/etc/debian_version")))
        distForm->addRow(tr("PPA:"),
                         valueLabel(QString::fromLatin1(APP_PPA)));
#endif
    const QString homepageUrl = QString::fromLatin1(APP_HOMEPAGE);
    auto* srcLabel = new QLabel(
        QStringLiteral("<a href=\"%1\">%1</a>").arg(homepageUrl),
        distGroup);
    srcLabel->setTextFormat(Qt::RichText);
    srcLabel->setOpenExternalLinks(true);
    srcLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
    distForm->addRow(tr("Source:"), srcLabel);
    const QString issuesUrl = QString::fromLatin1(APP_ISSUES_URL);
    auto* bugLabel = new QLabel(
        QStringLiteral("<a href=\"%1\">%1</a>").arg(issuesUrl),
        distGroup);
    bugLabel->setTextFormat(Qt::RichText);
    bugLabel->setOpenExternalLinks(true);
    bugLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
    distForm->addRow(tr("Bug reports:"), bugLabel);
    layout->addWidget(distGroup);

    layout->addStretch();

    m_tabs->addTab(page, tr("About"));
}

// --- Load / apply --------------------------------------------------

void SettingsDialog::loadCurrentValues() {
    m_suppressDirty = true;

    const QString lang = getStr(keyLanguage(), QStringLiteral("en"));
    const int langIdx = m_language->findData(lang);
    m_language->setCurrentIndex(langIdx >= 0 ? langIdx : 0);

    const QString theme =
        getStr(keyThemeVariant(), QStringLiteral("dark"));
    const int themeIdx = m_themeVariant->findData(theme);
    m_themeVariant->setCurrentIndex(themeIdx >= 0 ? themeIdx : 0);

    m_defaultCwd->setText(getStr(keyDefaultCwd(), QDir::homePath()));
    m_confirmSessionDelete->setChecked(
        getBool(keyConfirmSessionDelete(), true));
    m_confirmProjectDelete->setChecked(
        getBool(keyConfirmProjectDelete(), true));

    const QString pSort =
        getStr(keyProjectSortOrder(), QStringLiteral("last_used"));
    const int pSortIdx = m_projectSortOrder->findData(pSort);
    m_projectSortOrder->setCurrentIndex(pSortIdx >= 0 ? pSortIdx : 0);

    const QString sSort =
        getStr(keySessionSortOrder(), QStringLiteral("last_used"));
    const int sSortIdx = m_sessionSortOrder->findData(sSort);
    m_sessionSortOrder->setCurrentIndex(sSortIdx >= 0 ? sSortIdx : 0);

    QFont f;
    f.setFamily(getStr(keyFontFamily(),
                       EditorPaneWidget::defaultFontFamily()));
    f.setStyleHint(QFont::Monospace);
    m_fontFamily->setCurrentFont(f);
    m_fontSize->setValue(getInt(keyFontSize(),
                                EditorPaneWidget::kDefaultFontSize));
    m_tabWidth->setValue(getInt(keyTabWidth(),
                                EditorPaneWidget::kDefaultTabWidth));
    m_insertSpaces->setChecked(getBool(keyInsertSpaces(), true));

    const QString wrap =
        getStr(keyLineWrapMode(), QStringLiteral("nowrap"));
    const int wrapIdx = m_lineWrapMode->findData(wrap);
    m_lineWrapMode->setCurrentIndex(wrapIdx >= 0 ? wrapIdx : 0);

    m_lineNumbers->setChecked(getBool(keyLineNumbers(), true));
    m_highlightCurrentLine->setChecked(
        getBool(keyHighlightCurrentLine(), true));
    m_chatInputMaxRows->setValue(getInt(keyChatInputMaxRows(),
                                        MainWindow::kDefaultChatInputMaxRows));
    m_chatInputMinRows->setValue(getInt(keyChatInputMinRows(),
                                        MainWindow::kDefaultChatInputMinRows));
    m_activationDwellMs->setValue(
        getInt(keyActivationDwellMs(), MainWindow::kDefaultActivationDwellMs));

    m_suppressDirty = false;
    m_dirty = false;
    if (m_applyButton) m_applyButton->setEnabled(false);
}

bool SettingsDialog::validatePending() {
    const QString cwd = m_defaultCwd->text().trimmed();
    if (cwd.isEmpty()) return false;
    const QFileInfo fi(cwd);
    if (!fi.exists() || !fi.isDir()) {
        m_tabs->setCurrentIndex(0);
        m_defaultCwd->setFocus();
        m_defaultCwd->selectAll();
        return false;
    }
    return true;
}

bool SettingsDialog::applyPending() {
    if (!m_dirty) return true;
    if (!validatePending()) {
        shake();
        return false;
    }

    putStr(keyLanguage(), m_language->currentData().toString());
    putStr(keyThemeVariant(), m_themeVariant->currentData().toString());
    putStr(keyDefaultCwd(), m_defaultCwd->text().trimmed());
    putBool(keyConfirmSessionDelete(),
            m_confirmSessionDelete->isChecked());
    putBool(keyConfirmProjectDelete(),
            m_confirmProjectDelete->isChecked());
    putStr(keyProjectSortOrder(),
           m_projectSortOrder->currentData().toString());
    putStr(keySessionSortOrder(),
           m_sessionSortOrder->currentData().toString());

    putStr(keyFontFamily(), m_fontFamily->currentFont().family());
    putInt(keyFontSize(), m_fontSize->value());
    putInt(keyTabWidth(), m_tabWidth->value());
    putBool(keyInsertSpaces(), m_insertSpaces->isChecked());
    putStr(keyLineWrapMode(), m_lineWrapMode->currentData().toString());
    putBool(keyLineNumbers(), m_lineNumbers->isChecked());
    putBool(keyHighlightCurrentLine(),
            m_highlightCurrentLine->isChecked());
    putInt(keyChatInputMaxRows(), m_chatInputMaxRows->value());
    putInt(keyChatInputMinRows(), m_chatInputMinRows->value());
    putInt(keyActivationDwellMs(), m_activationDwellMs->value());

    m_dirty = false;
    if (m_applyButton) m_applyButton->setEnabled(false);
    return true;
}

// --- Slots --------------------------------------------------------

void SettingsDialog::onControlChanged() {
    if (m_suppressDirty) return;
    markDirty();
}

void SettingsDialog::markDirty() {
    m_dirty = true;
    if (m_applyButton) m_applyButton->setEnabled(true);
}

void SettingsDialog::onBrowseDefaultCwd() {
    const QString start = m_defaultCwd->text().trimmed().isEmpty()
                              ? QDir::homePath()
                              : m_defaultCwd->text().trimmed();
    const QString chosen = QFileDialog::getExistingDirectory(
        this, tr("Default new-session directory"), start);
    if (chosen.isEmpty()) return;
    m_defaultCwd->setText(chosen);
    markDirty();
}

void SettingsDialog::onResetWindowLayout() {
    // The dialog doesn't own the main window, so we surface a
    // marker setting instead: MainWindow watches for this key
    // in settingChanged and runs its own resetWindowLayout()
    // routine. Keeps the dialog decoupled from the shell.
    Persistence::instance().setSetting(
        QStringLiteral("window.reset_requested"),
        QByteArrayLiteral("1"));
}
