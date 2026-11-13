// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_EDITOR_GLSL_SCENESCRIPT_EDITOR_WIDGET_HPP
#define WAYWAL_STUDIO_EDITOR_GLSL_SCENESCRIPT_EDITOR_WIDGET_HPP

#include "studio/core/ipc_client.hpp"
#include "waywal/ipc_proto_authoring.h"
#include "waywal/shader_parser.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QString>
#include <QSyntaxHighlighter>
#include <QThread>
#include <QTimer>
#include <QWidget>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace waywal::studio {

enum class EditorLanguageMode : uint8_t {
    GlslEs310Fragment = 0,
    GlslEs310Vertex = 1,
    SceneScriptJs = 2,
};

struct EditorDiagnostic {
    uint32_t line{1};
    uint32_t column{1};
    bool isWarning{false};
    QString message;
};

struct ShaderSourceSpan {
    QString symbolName;
    QString fieldKey; /* "default" or "combo_default" */
    int startOffset{0};
    int endOffset{0};
};

class GlslSceneScriptHighlighter final : public QSyntaxHighlighter {
    Q_OBJECT

  public:
    explicit GlslSceneScriptHighlighter(QTextDocument *doc = nullptr);
    ~GlslSceneScriptHighlighter() override = default;

    void setLanguageMode(EditorLanguageMode mode);
    [[nodiscard]] EditorLanguageMode languageMode() const noexcept { return m_mode; }

  protected:
    void highlightBlock(const QString &text) override;

  private:
    struct HighlightFormats;

    void highlightCommentTag(const QString &text, int cStart, int &state,
                             const HighlightFormats &fmt);
    bool consumeLeadingBlockComment(const QString &text, int &i, int &state,
                                    const HighlightFormats &fmt);
    bool consumeLeadingTemplateLiteral(const QString &text, int &i, int &state,
                                       const HighlightFormats &fmt);
    void consumeLeadingDirective(const QString &text, int &i, int &state,
                                 const HighlightFormats &fmt);
    int scanQuotedToken(const QString &text, int i, int &state, const HighlightFormats &fmt,
                        bool &closedOut);
    int scanNumberToken(const QString &text, int i, const HighlightFormats &fmt);
    int scanIdentifierToken(const QString &text, int i, const HighlightFormats &fmt);

    EditorLanguageMode m_mode{EditorLanguageMode::GlslEs310Fragment};
};

class CodeGutterWidget final : public QWidget {
    Q_OBJECT

  public:
    explicit CodeGutterWidget(QPlainTextEdit *editor);
    ~CodeGutterWidget() override = default;

    [[nodiscard]] QSize sizeHint() const override;
    void setDiagnostics(const std::vector<EditorDiagnostic> &diags);

  protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

  private:
    QPlainTextEdit *m_editor{nullptr};
    std::vector<EditorDiagnostic> m_diagnostics;
};

class GlslSceneScriptEditorWidget : public QWidget {
    Q_OBJECT

  public:
    static constexpr int kDebounceValidationMs = 275;

    explicit GlslSceneScriptEditorWidget(QWidget *parent = nullptr);
    ~GlslSceneScriptEditorWidget() override;

    void setIpcClient(IpcClient *client) noexcept { m_ipcClient = client; }
    [[nodiscard]] IpcClient *ipcClient() const noexcept { return m_ipcClient; }

    void setTargetOutput(const QString &output) { m_targetOutput = output; }
    [[nodiscard]] QString targetOutput() const noexcept { return m_targetOutput; }

    void setSharedOpenGLContext(QOpenGLContext *shareCtx) noexcept { m_shareContext = shareCtx; }

    void openDocument(EditorLanguageMode mode, int32_t objectId, uint32_t passOrScriptSlot,
                      const QString &title, const QString &sourceText);
    [[nodiscard]] QString sourceText() const;
    [[nodiscard]] EditorLanguageMode languageMode() const noexcept { return m_languageMode; }
    [[nodiscard]] const std::vector<EditorDiagnostic> &diagnostics() const noexcept
    {
        return m_diagnostics;
    }

    /* Zero-cursor-jump bidirectional patch when PropertyInspectorWidget adjusts a uniform/combo */
    bool patchUniformOrComboDefaultInSource(const QString &symbolName, const QString &formattedVal);

  public slots:
    void triggerImmediateValidation();

  signals:
    void shaderMetadataExtracted(const waywal_shader_meta_t &meta, const QString &rawComboJson);
    void diagnosticsUpdated(const std::vector<EditorDiagnostic> &diagnostics);
    void liveHotReloadApplied(bool success, uint32_t compileTimeUs);
    void statusMessage(const QString &msg);

  protected:
    void showEvent(QShowEvent *event) override;

  private:
    void ensureUiInitialized();
    void stopValidationWorker();
    void ensureValidationContext();
    void onEditorTextChanged();
    void runDebouncedValidation();
    void applyValidationResult(uint64_t generation, std::vector<EditorDiagnostic> diags);
    void rebuildSourceSpans(const QString &src);

    bool m_uiInitialized{false};
    bool m_syncingFromInspector{false};
    bool m_liveHotReload{true};
    bool m_syncDefaultsToCode{true};

    EditorLanguageMode m_languageMode{EditorLanguageMode::GlslEs310Fragment};
    QString m_targetOutput{QStringLiteral("*")};
    int32_t m_objectId{-1};
    uint32_t m_slotIndex{0};
    std::atomic<uint64_t> m_editGeneration{0};
    QString m_lastEmittedMetaSig;

    IpcClient *m_ipcClient{nullptr};
    QPointer<QOpenGLContext> m_shareContext;
    std::unique_ptr<QOpenGLContext> m_workerGlContext;
    std::unique_ptr<QOffscreenSurface> m_workerSurface;
    std::unique_ptr<QThread> m_validationThread;
    std::atomic<bool> m_validationBusy{false};

    QPlainTextEdit *m_codeEdit{nullptr};
    GlslSceneScriptHighlighter *m_highlighter{nullptr};
    CodeGutterWidget *m_gutter{nullptr};
    QTimer *m_debounceTimer{nullptr};
    QComboBox *m_languageCombo{nullptr};
    QCheckBox *m_hotReloadCheck{nullptr};
    QCheckBox *m_syncDefaultsCheck{nullptr};
    QPushButton *m_validateBtn{nullptr};
    QLabel *m_statusPill{nullptr};

    std::vector<EditorDiagnostic> m_diagnostics;
    std::vector<ShaderSourceSpan> m_sourceSpans;
};

} // namespace waywal::studio

#endif // WAYWAL_STUDIO_EDITOR_GLSL_SCENESCRIPT_EDITOR_WIDGET_HPP
