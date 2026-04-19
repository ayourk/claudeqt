// SPDX-License-Identifier: GPL-3.0-only
//
// CodeEditor — QPlainTextEdit subclass. Gedit-class competent
// editor: KSyntaxHighlighting attached to the document,
// LineNumberArea child widget, current-line highlight, dirty
// tracking via modificationChanged, save and save-as to disk,
// external-modification detection on focus-in.
//
// Deliberately out of scope for this release: find/replace,
// go-to-line, multi-cursor, minimap, LSP, tree-sitter,
// .editorconfig, vim bindings.
//
// Repository ownership: the KSyntaxHighlighting::Repository is
// expensive to construct (loads every XML syntax file under
// /usr/share/org.kde.syntax-highlighting/) and is per-application
// thread-safe for reads. EditorPaneWidget owns the single
// instance and hands a raw pointer to each CodeEditor — the
// editor does NOT own the repository and must not delete it.

#pragma once

#include <QByteArray>
#include <QPlainTextEdit>
#include <QString>

class QPaintEvent;
class QResizeEvent;
class QFocusEvent;
class QTimer;

namespace KSyntaxHighlighting {
class Repository;
class SyntaxHighlighter;
}  // namespace KSyntaxHighlighting

class LineNumberArea;

class CodeEditor : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit CodeEditor(KSyntaxHighlighting::Repository* repository,
                        QWidget* parent = nullptr);
    ~CodeEditor() override;

    // File binding — setFilePath reconfigures the KSH definition
    // by filename and (if ambiguous) by content sniff per §6.2.
    // Does NOT touch the document contents; callers that want to
    // load a file from disk should use loadFromFile() which calls
    // setFilePath and then reads the bytes.
    void setFilePath(const QString& path);
    QString filePath() const { return m_filePath; }

    // Read `path` from disk into the document and update the
    // internal saved_mtime tracking. Returns false on I/O error
    // (file missing, permission denied, etc.); the document is
    // left untouched on failure. UTF-8 only — non-UTF-8 encoding
    // detection is future work.
    bool loadFromFile(const QString& path);

    // Write the document to m_filePath, update saved_mtime,
    // clear the modified flag. Returns false on I/O error. If
    // m_filePath is empty, returns false without writing — the
    // caller should route through saveAs instead.
    bool save();

    // Write to `newPath`, update m_filePath, rewire KSH for the
    // new filename, update saved_mtime, clear modified.
    bool saveAs(const QString& newPath);

    // Persistence hooks. The buffer id ties this editor to a
    // row in the persistence layer's `buffers` table; when set,
    // dirty state is stashed via Persistence::stashDirtyBuffer
    // on a 1s debounce. Initially unset (no session context
    // until tree wiring connects it), so the editor
    // still works fully against disk without any DB round-trips.
    void setBufferId(qint64 id) { m_bufferId = id; }
    qint64 bufferId() const { return m_bufferId; }

    void setTabPosition(int pos) { m_tabPosition = pos; }
    int tabPosition() const { return m_tabPosition; }

    bool stashTimerActive() const;
    QByteArray lastStashedContent() const { return m_lastStashedContent; }

    void applyDirtyContent(const QByteArray& content);

    // Cached mtime at last successful load/save. Used by
    // focusInEvent to detect external modifications.
    qint64 savedMtime() const { return m_savedMtime; }

    // Expose the LineNumberArea paint entry point. Called from
    // LineNumberArea::paintEvent via a trampoline (the child
    // widget forwards here so this class owns the drawing logic).
    int lineNumberAreaWidth() const;
    void lineNumberAreaPaintEvent(QPaintEvent* event);

signals:
    // Emitted when setFilePath changes m_filePath. The
    // EditorPaneWidget listens to retarget tab titles/tooltips.
    void filePathChanged(const QString& newPath);

    // Emitted when the dirty state flips. True = buffer has
    // unsaved edits, false = buffer matches disk.
    void dirtyStateChanged(bool dirty);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;

private slots:
    void onBlockCountChanged(int newBlockCount);
    void onUpdateRequest(const QRect& rect, int dy);
    void onCursorPositionChanged();
    void onModificationChanged(bool modified);
    void flushDirtyStash();

private:
    void rewireHighlighter();
    void checkExternalModification();

    KSyntaxHighlighting::Repository* m_repository = nullptr;  // not owned
    KSyntaxHighlighting::SyntaxHighlighter* m_highlighter = nullptr;
    LineNumberArea* m_lineNumberArea = nullptr;

    QString m_filePath;
    qint64 m_savedMtime = 0;
    qint64 m_bufferId = 0;
    int m_tabPosition = 0;

    QTimer* m_dirtyStashTimer = nullptr;
    QByteArray m_lastStashedContent;

    // Suppresses the external-modification prompt while save()
    // is running — save() rewrites the file, which would
    // otherwise look like an external change the next time the
    // editor regains focus.
    bool m_suppressExternalCheck = false;
};

// Line number gutter — a child widget that forwards its
// paintEvent back into CodeEditor so the drawing code lives in
// one place (standard pattern from Qt's CodeEditor example).
class LineNumberArea : public QWidget {
public:
    explicit LineNumberArea(CodeEditor* editor);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    CodeEditor* m_editor;
};
