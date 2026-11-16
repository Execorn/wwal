// SPDX-License-Identifier: GPL-3.0-or-later
#include "glsl_scenescript_editor_widget.hpp"

#include "waywal/we_script.h"
#include "waywal/we_shader.h"

#include <QAbstractTextDocumentLayout>
#include <QAccessible>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMetaObject>
#include <QMouseEvent>
#include <QOpenGLFunctions>
#include <QPainter>
#include <QPaintEvent>
#include <QPointer>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSurfaceFormat>
#include <QTextBlock>
#include <QTextCursor>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <ranges>
#include <string_view>

namespace waywal::studio {

namespace {

constexpr int kStateBlockComment = 1 << 0;
constexpr int kStateUniformBlock = 1 << 1;
constexpr int kStateTemplateLit = 1 << 2;

constexpr std::array<std::string_view, 37> kGlslKeywords = {{
    "attribute", "bool",     "break",   "buffer",  "case",      "centroid", "coherent",
    "const",     "continue", "default", "discard", "do",        "else",     "false",
    "flat",      "for",      "highp",   "if",      "in",        "inout",    "invariant",
    "layout",    "lowp",     "mediump", "out",     "precision", "readonly", "restrict",
    "return",    "shared",   "smooth",  "struct",  "switch",    "true",     "uniform",
    "varying",   "void",
}};
static_assert(std::ranges::is_sorted(kGlslKeywords));

constexpr std::array<std::string_view, 55> kGlslBuiltins = {{
    "abs",
    "atan",
    "atan2",
    "atomic_uint",
    "bvec2",
    "bvec3",
    "bvec4",
    "ceil",
    "clamp",
    "cos",
    "cross",
    "dFdx",
    "dFdy",
    "distance",
    "dot",
    "dvec2",
    "dvec3",
    "dvec4",
    "exp",
    "float",
    "floor",
    "fract",
    "fwidth",
    "gl_FragColor",
    "gl_FragCoord",
    "gl_GlobalInvocationID",
    "gl_LocalInvocationID",
    "gl_Position",
    "gl_WorkGroupID",
    "image2D",
    "int",
    "ivec2",
    "ivec3",
    "ivec4",
    "length",
    "log",
    "mat2",
    "mat3",
    "mat4",
    "max",
    "min",
    "mix",
    "mod",
    "normalize",
    "pow",
    "reflect",
    "refract",
    "sampler2D",
    "samplerCube",
    "sin",
    "smoothstep",
    "sqrt",
    "step",
    "tan",
    "texture",
}};
static_assert(std::ranges::is_sorted(kGlslBuiltins));

constexpr std::array<std::string_view, 17> kGlslUniformBuiltins = {{
    "a_Position",
    "a_TexCoord",
    "g_ModelMatrix",
    "g_ModelViewProjectionMatrix",
    "g_Texture0",
    "g_Texture1",
    "g_Texture2",
    "g_Texture3",
    "g_Time",
    "iChannel0",
    "iChannel1",
    "iFrame",
    "iMouse",
    "iResolution",
    "iTime",
    "u_Resolution",
    "u_Time",
}};
static_assert(std::ranges::is_sorted(kGlslUniformBuiltins));

constexpr std::array<std::string_view, 37> kJsKeywords = {{
    "async",    "await",   "break",    "case",   "catch",     "class",  "const",      "continue",
    "debugger", "default", "delete",   "do",     "else",      "export", "extends",    "false",
    "finally",  "for",     "function", "if",     "import",    "in",     "instanceof", "let",
    "new",      "null",    "of",       "return", "static",    "super",  "switch",     "this",
    "throw",    "true",    "try",      "typeof", "undefined",
}};
static_assert(std::ranges::is_sorted(kJsKeywords));

constexpr std::array<std::string_view, 21> kJsBuiltins = {{
    "Mat4",    "Math",       "Vec2",       "Vec3",
    "Vec4",    "WEMath",     "WEVector",   "applyUserProperties",
    "console", "cursorDown", "cursorMove", "cursorUp",
    "engine",  "init",       "input",      "scriptProperties",
    "shared",  "thisLayer",  "thisObject", "thisScene",
    "update",
}};
static_assert(std::ranges::is_sorted(kJsBuiltins));

[[nodiscard]] bool isIdentStart(QChar c) noexcept { return c.isLetter() || c == QLatin1Char('_'); }

[[nodiscard]] bool isIdentChar(QChar c) noexcept
{
    return c.isLetterOrNumber() || c == QLatin1Char('_');
}

[[nodiscard]] bool matchesSortedWord(QStringView word, const auto &table) noexcept
{
    const qsizetype wlen = word.size();
    if (wlen <= 0 || wlen > 32) {
        return false;
    }
    char asciiBuf[32];
    for (qsizetype k = 0; k < wlen; ++k) {
        const char16_t u = word[k].unicode();
        if (u > 127) {
            return false;
        }
        asciiBuf[k] = static_cast<char>(u);
    }
    const std::string_view sv(asciiBuf, static_cast<size_t>(wlen));
    return std::ranges::binary_search(table, sv);
}

void sendHotReloadCodePacket(IpcClient *client, const QString &targetOutput,
                             EditorLanguageMode mode, int32_t objectId, uint32_t slotIndex,
                             const QByteArray &utf8Source)
{
    if (!client || static_cast<size_t>(utf8Source.size()) > WAYWAL_IPC_MAX_CODE_BYTES) {
        return;
    }

    waywal_ipc_req_hot_reload_code_t req{};
    const QByteArray outUtf8 = targetOutput.toUtf8();
    const size_t copyLen =
        std::min<size_t>(static_cast<size_t>(outUtf8.size()), sizeof(req.target_output) - 1);
    if (copyLen > 0) {
        std::memcpy(req.target_output, outUtf8.constData(), copyLen);
    }
    req.code_kind = static_cast<uint32_t>(mode);
    req.flags = 0x2u | 0x4u; /* bit1=HOT_SWAP | bit2=EXTRACT_META */
    req.object_id = objectId;
    if (mode == EditorLanguageMode::SceneScriptJs) {
        req.pass_index = UINT32_MAX;
        req.script_slot = slotIndex;
    } else {
        req.pass_index = slotIndex;
        req.script_slot = 0u;
    }
    req.source_len = static_cast<uint32_t>(utf8Source.size());

    (void)client->sendHotReloadCode(req, utf8Source);
}

/* Subclass QPlainTextEdit in .cpp so CodeGutterWidget can access protected viewport metrics
 * without changing m_codeEdit's public QPlainTextEdit* member type. */
class EditorPlainTextEdit final : public QPlainTextEdit {
  public:
    explicit EditorPlainTextEdit(QWidget *parent = nullptr) : QPlainTextEdit(parent) {}

    void setGutterWidget(CodeGutterWidget *gutter) noexcept { m_gutter = gutter; }

    void updateGutterMargins()
    {
        if (!m_gutter) {
            return;
        }
        setViewportMargins(m_gutter->sizeHint().width(), 0, 0, 0);
    }

    [[nodiscard]] QTextBlock publicFirstVisibleBlock() const { return firstVisibleBlock(); }
    [[nodiscard]] QRectF publicBlockBoundingGeometry(const QTextBlock &block) const
    {
        return blockBoundingGeometry(block);
    }
    [[nodiscard]] QPointF publicContentOffset() const { return contentOffset(); }

  protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QPlainTextEdit::resizeEvent(event);
        if (m_gutter) {
            const QRect cr = contentsRect();
            m_gutter->setGeometry(cr.left(), cr.top(), m_gutter->sizeHint().width(), cr.height());
        }
    }

  private:
    CodeGutterWidget *m_gutter{nullptr};
};

/* Owned as a QObject child of GlslSceneScriptEditorWidget so validation threads can coalesce
 * rapid edits without changing the header's member layout. */
class ValidationMailbox final : public QObject {
  public:
    explicit ValidationMailbox(QObject *parent = nullptr) : QObject(parent)
    {
        setObjectName(QStringLiteral("ValidationMailbox"));
    }

    struct Job {
        uint64_t generation{0};
        EditorLanguageMode mode{EditorLanguageMode::GlslEs310Fragment};
        QString source;
    };

    void submit(uint64_t generation, EditorLanguageMode mode, QString source)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pending.generation = generation;
            m_pending.mode = mode;
            m_pending.source = std::move(source);
            m_hasPending = true;
        }
        m_cv.notify_one();
    }

    void shutdown()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopRequested = true;
        }
        m_cv.notify_all();
    }

    void reset()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopRequested = false;
    }

    bool waitNext(Job &outJob)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this]() { return m_hasPending || m_stopRequested; });
        if (m_stopRequested || !m_hasPending) {
            return false;
        }
        outJob = std::move(m_pending);
        m_hasPending = false;
        return true;
    }

  private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    Job m_pending;
    bool m_hasPending{false};
    bool m_stopRequested{false};
};

[[nodiscard]] int countLines(const char *s) noexcept
{
    if (!s) {
        return 0;
    }
    int count = 0;
    for (const char *p = s; *p != '\0'; ++p) {
        if (*p == '\n') {
            ++count;
        }
    }
    return count;
}

[[nodiscard]] int basePreambleLines(bool isFrag)
{
    static const int kFragLines = []() {
        char *prep = we_shader_preprocess_stage("", true, nullptr, nullptr, 0, 0, false, nullptr);
        int lines = countLines(prep);
        std::free(prep);
        return lines;
    }();
    static const int kVertLines = []() {
        char *prep = we_shader_preprocess_stage("", false, nullptr, nullptr, 0, 0, false, nullptr);
        int lines = countLines(prep);
        std::free(prep);
        return lines;
    }();
    return isFrag ? kFragLines : kVertLines;
}

[[nodiscard]] bool isStandaloneNativeGles(const QString &src)
{
    if (!src.contains(QLatin1String("#version 3"))) {
        return false;
    }
    static constexpr std::array<QLatin1StringView, 11> kWeTokens = {{
        QLatin1StringView("#include"),
        QLatin1StringView("#require"),
        QLatin1StringView("gl_FragColor"),
        QLatin1StringView("varying "),
        QLatin1StringView("attribute "),
        QLatin1StringView("float2"),
        QLatin1StringView("float3"),
        QLatin1StringView("float4"),
        QLatin1StringView("texSample2D"),
        QLatin1StringView("tex2D"),
        QLatin1StringView("mul("),
    }};
    for (const auto tok : kWeTokens) {
        if (src.contains(tok)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] QString normalizeIncludeName(QString inc)
{
    inc = inc.trimmed();
    while (inc.startsWith(QLatin1Char('/')) || inc.startsWith(QLatin1Char('\\'))) {
        inc.remove(0, 1);
    }
    if (inc.startsWith(QLatin1String("./")) || inc.startsWith(QLatin1String(".\\"))) {
        inc.remove(0, 2);
    }
    if (inc.startsWith(QLatin1String("shaders/"), Qt::CaseInsensitive) ||
        inc.startsWith(QLatin1String("shaders\\"), Qt::CaseInsensitive)) {
        inc.remove(0, 8);
    }
    return inc.toLower();
}

struct LineMapEntry {
    uint32_t userLine{1};
    QString includeName;
};

struct ExpandedSourceLine {
    uint32_t userLine{1};
    QString includeName;
    QString trimmedText;
};

void appendExpandedLines(const QString &srcText, uint32_t originUserLine, const QString &incLabel,
                         std::vector<QString> &seenIncludes,
                         std::vector<ExpandedSourceLine> &expanded, bool topLevel)
{
    const QStringList lines = srcText.split(QLatin1Char('\n'));
    const int lineCount = (!topLevel && !lines.isEmpty() && lines.last().isEmpty())
                              ? static_cast<int>(lines.size() - 1)
                              : static_cast<int>(lines.size());
    for (int idx = 0; idx < lineCount; ++idx) {
        const uint32_t uLine = topLevel ? static_cast<uint32_t>(idx + 1) : originUserLine;
        const QString trimmed = lines[idx].trimmed();

        if (trimmed.startsWith(QLatin1String("#version"))) {
            continue;
        }

        if (trimmed.startsWith(QLatin1String("#include")) ||
            trimmed.startsWith(QLatin1String("#require"))) {
            const int q1 = trimmed.indexOf(QLatin1Char('"'));
            const int q2 = (q1 >= 0) ? trimmed.indexOf(QLatin1Char('"'), q1 + 1) : -1;
            if (q1 >= 0 && q2 > q1 + 1) {
                const QString rawInc = trimmed.mid(q1 + 1, q2 - q1 - 1);
                const QString normInc = normalizeIncludeName(rawInc);
                if (!normInc.isEmpty() &&
                    std::ranges::find(seenIncludes, normInc) == seenIncludes.end()) {
                    seenIncludes.push_back(normInc);
                    const QByteArray incBa = rawInc.toUtf8();
                    const char *builtin = we_shader_get_builtin(incBa.constData());
                    if (builtin) {
                        appendExpandedLines(QString::fromUtf8(builtin), uLine, rawInc, seenIncludes,
                                            expanded, false);
                    }
                }
            }
            continue;
        }

        expanded.push_back(ExpandedSourceLine{uLine, incLabel, trimmed});
    }
}

std::vector<LineMapEntry> buildPreprocessedLineMap(const QString &rawSrc,
                                                   const QByteArray &preprocessedGlsl, bool isFrag,
                                                   uint32_t comboCount)
{
    std::vector<LineMapEntry> map;
    const int preambleLines = basePreambleLines(isFrag) + static_cast<int>(comboCount) * 3;

    std::vector<QString> seenIncludes;
    std::vector<ExpandedSourceLine> expLines;
    expLines.reserve(static_cast<size_t>(rawSrc.count(QLatin1Char('\n')) + 64));
    appendExpandedLines(rawSrc, 1u, QString(), seenIncludes, expLines, true);

    if (preprocessedGlsl.isEmpty()) {
        map.reserve(static_cast<size_t>(preambleLines) + expLines.size());
        for (int i = 0; i < preambleLines; ++i) {
            map.push_back(LineMapEntry{1u, QString()});
        }
        for (const auto &e : expLines) {
            map.push_back(LineMapEntry{e.userLine, e.includeName});
        }
        return map;
    }

    const QString prepStr = QString::fromUtf8(preprocessedGlsl);
    const QStringList prepLines = prepStr.split(QLatin1Char('\n'));
    const int totalPrep = static_cast<int>(prepLines.size());
    map.reserve(static_cast<size_t>(totalPrep));

    const int clampedPreamble = std::min(preambleLines, totalPrep);
    for (int i = 0; i < clampedPreamble; ++i) {
        map.push_back(LineMapEntry{1u, QString()});
    }

    // Align transformed output lines against expanded user lines even when mul()-hoisting
    // introduces extra temporary lines or strips #define saturate wrappers.
    size_t expIdx = 0;
    for (int i = clampedPreamble; i < totalPrep; ++i) {
        const QString trimmed = prepLines[i].trimmed();
        while (expIdx < expLines.size() &&
               expLines[expIdx].trimmedText.startsWith(QLatin1String("#define saturate"))) {
            ++expIdx;
        }

        const uint32_t curLine = (expIdx < expLines.size())
                                     ? expLines[expIdx].userLine
                                     : (expLines.empty() ? 1u : expLines.back().userLine);
        const QString curInc =
            (expIdx < expLines.size()) ? expLines[expIdx].includeName : QString();
        map.push_back(LineMapEntry{curLine, curInc});

        const bool isHoistedLine = trimmed.contains(QLatin1String("_we_mul_")) &&
                                   (expIdx < expLines.size() &&
                                    expLines[expIdx].trimmedText.contains(QLatin1String("mul(")) &&
                                    !trimmed.startsWith(expLines[expIdx].trimmedText.left(6)));
        if (isHoistedLine) {
            continue;
        }
        if (expIdx < expLines.size()) {
            ++expIdx;
        }
    }
    return map;
}

std::vector<EditorDiagnostic>
parseGlslInfoLog(const QString &log, const std::vector<LineMapEntry> &lineMap, uint32_t maxUserLine)
{
    std::vector<EditorDiagnostic> out;
    if (log.trimmed().isEmpty()) {
        return out;
    }

    static const QRegularExpression reKhronos(
        QStringLiteral(R"(^(?:ERROR|WARNING):\s*\d+:(\d+)(?::(\d+))?:\s*(.+)$)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression reMesa(
        QStringLiteral(R"(^\d+:(\d+)(?:\((\d+)\))?:\s*(error|warning):\s*(.+)$)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression reNvidia(
        QStringLiteral(R"(^\d+\((\d+)\)\s*:\s*(error|warning)\s+[A-Za-z0-9_]+:\s*(.+)$)"),
        QRegularExpression::CaseInsensitiveOption);

    const QStringList logLines = log.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &rawLine : logLines) {
        const QString lineStr = rawLine.trimmed();
        if (lineStr.isEmpty()) {
            continue;
        }

        uint32_t parsedLine = 1;
        uint32_t parsedCol = 1;
        bool isWarn = lineStr.contains(QLatin1String("warning"), Qt::CaseInsensitive) &&
                      !lineStr.contains(QLatin1String("error"), Qt::CaseInsensitive);
        QString msg = lineStr;

        if (const auto m = reMesa.match(lineStr); m.hasMatch()) {
            parsedLine = std::max(1u, m.captured(1).toUInt());
            if (!m.captured(2).isEmpty()) {
                parsedCol = std::max(1u, m.captured(2).toUInt());
            }
            isWarn = m.captured(3).compare(QLatin1String("warning"), Qt::CaseInsensitive) == 0;
            msg = m.captured(4).trimmed();
        } else if (const auto m = reKhronos.match(lineStr); m.hasMatch()) {
            parsedLine = std::max(1u, m.captured(1).toUInt());
            if (!m.captured(2).isEmpty()) {
                parsedCol = std::max(1u, m.captured(2).toUInt());
            }
            isWarn = lineStr.startsWith(QLatin1String("WARNING:"), Qt::CaseInsensitive);
            msg = m.captured(3).trimmed();
        } else if (const auto m = reNvidia.match(lineStr); m.hasMatch()) {
            parsedLine = std::max(1u, m.captured(1).toUInt());
            isWarn = m.captured(2).compare(QLatin1String("warning"), Qt::CaseInsensitive) == 0;
            msg = m.captured(3).trimmed();
        } else if (!lineStr.contains(QLatin1String("error"), Qt::CaseInsensitive) &&
                   !lineStr.contains(QLatin1String("warning"), Qt::CaseInsensitive)) {
            continue;
        }

        uint32_t mappedLine = parsedLine;
        if (!lineMap.empty()) {
            const size_t idx = static_cast<size_t>(parsedLine - 1);
            if (idx < lineMap.size()) {
                mappedLine = lineMap[idx].userLine;
                if (!lineMap[idx].includeName.isEmpty()) {
                    msg = QStringLiteral("[%1] %2").arg(lineMap[idx].includeName, msg);
                }
            } else {
                mappedLine = maxUserLine;
            }
        }
        mappedLine = std::clamp(mappedLine, 1u, std::max(1u, maxUserLine));

        EditorDiagnostic diag;
        diag.line = mappedLine;
        diag.column = parsedCol;
        diag.isWarning = isWarn;
        diag.message = msg;
        out.push_back(std::move(diag));
    }

    return out;
}

std::vector<EditorDiagnostic> validateGlslStructuralFallback(const QString &src)
{
    std::vector<EditorDiagnostic> diags;
    if (src.trimmed().isEmpty()) {
        diags.push_back(EditorDiagnostic{1u, 1u, false, QStringLiteral("Shader source is empty")});
        return diags;
    }

    struct BracePos {
        QChar ch;
        uint32_t line;
        uint32_t col;
    };
    std::vector<BracePos> stack;
    bool inBlockComment = false;
    bool inLineComment = false;
    bool inDoubleQuote = false;
    bool inSingleQuote = false;
    bool escaped = false;
    uint32_t quoteStartLine = 1;
    uint32_t quoteStartCol = 1;
    uint32_t line = 1;
    uint32_t col = 1;

    for (int i = 0; i < src.size(); ++i) {
        const QChar c = src[i];
        if (c == QLatin1Char('\n')) {
            if (inDoubleQuote || inSingleQuote) {
                diags.push_back(EditorDiagnostic{quoteStartLine, quoteStartCol, false,
                                                 QStringLiteral("Unterminated string literal")});
                inDoubleQuote = false;
                inSingleQuote = false;
                escaped = false;
            }
            inLineComment = false;
            ++line;
            col = 1;
            continue;
        }
        if (inLineComment) {
            ++col;
            continue;
        }
        if (inBlockComment) {
            if (c == QLatin1Char('*') && i + 1 < src.size() && src[i + 1] == QLatin1Char('/')) {
                inBlockComment = false;
                ++i;
                col += 2;
                continue;
            }
            ++col;
            continue;
        }
        if (inDoubleQuote || inSingleQuote) {
            if (escaped) {
                escaped = false;
            } else if (c == QLatin1Char('\\')) {
                escaped = true;
            } else if ((inDoubleQuote && c == QLatin1Char('"')) ||
                       (inSingleQuote && c == QLatin1Char('\''))) {
                inDoubleQuote = false;
                inSingleQuote = false;
            }
            ++col;
            continue;
        }

        if (c == QLatin1Char('/') && i + 1 < src.size()) {
            if (src[i + 1] == QLatin1Char('/')) {
                inLineComment = true;
                ++i;
                col += 2;
                continue;
            }
            if (src[i + 1] == QLatin1Char('*')) {
                inBlockComment = true;
                ++i;
                col += 2;
                continue;
            }
        }
        if (c == QLatin1Char('"')) {
            inDoubleQuote = true;
            quoteStartLine = line;
            quoteStartCol = col;
            ++col;
            continue;
        }
        if (c == QLatin1Char('\'')) {
            inSingleQuote = true;
            quoteStartLine = line;
            quoteStartCol = col;
            ++col;
            continue;
        }

        if (c == QLatin1Char('{') || c == QLatin1Char('(') || c == QLatin1Char('[')) {
            stack.push_back(BracePos{c, line, col});
        } else if (c == QLatin1Char('}') || c == QLatin1Char(')') || c == QLatin1Char(']')) {
            const QChar expected =
                (c == QLatin1Char('}'))
                    ? QLatin1Char('{')
                    : ((c == QLatin1Char(')')) ? QLatin1Char('(') : QLatin1Char('['));
            if (stack.empty() || stack.back().ch != expected) {
                diags.push_back(
                    EditorDiagnostic{line, col, false, QStringLiteral("Unmatched '%1'").arg(c)});
                // Pop up to matching opener if nested mismatch allows continued error recovery.
                for (size_t k = stack.size(); k > 0; --k) {
                    if (stack[k - 1].ch == expected) {
                        stack.resize(k - 1);
                        break;
                    }
                }
            } else {
                stack.pop_back();
            }
        }
        ++col;
    }

    if (inBlockComment) {
        diags.push_back(
            EditorDiagnostic{line, col, false, QStringLiteral("Unterminated block comment")});
    }
    if (inDoubleQuote || inSingleQuote) {
        diags.push_back(EditorDiagnostic{quoteStartLine, quoteStartCol, false,
                                         QStringLiteral("Unterminated string literal")});
    }
    for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
        diags.push_back(EditorDiagnostic{it->line, it->col, false,
                                         QStringLiteral("Unclosed '%1'").arg(it->ch)});
    }
    return diags;
}

waywal_shader_uniform_type_t mapWeTypeToWaywalType(const char *typeStr, const char *nameStr,
                                                   uint32_t &outComponents)
{
    if (std::strcmp(typeStr, "bool") == 0) {
        outComponents = 1;
        return WAYWAL_UNIFORM_TYPE_BOOL;
    }
    if (std::strcmp(typeStr, "int") == 0) {
        outComponents = 1;
        return WAYWAL_UNIFORM_TYPE_INT;
    }
    if (std::strcmp(typeStr, "vec2") == 0 || std::strcmp(typeStr, "float2") == 0) {
        outComponents = 2;
        return WAYWAL_UNIFORM_TYPE_VEC2;
    }
    if (std::strcmp(typeStr, "vec3") == 0 || std::strcmp(typeStr, "float3") == 0) {
        outComponents = 3;
        const QString lower = QString::fromUtf8(nameStr).toLower();
        if (lower.contains(QLatin1String("color")) || lower.contains(QLatin1String("tint")) ||
            lower.contains(QLatin1String("rgb"))) {
            return WAYWAL_UNIFORM_TYPE_COLOR;
        }
        return WAYWAL_UNIFORM_TYPE_VEC3;
    }
    if (std::strcmp(typeStr, "vec4") == 0 || std::strcmp(typeStr, "float4") == 0) {
        outComponents = 4;
        return WAYWAL_UNIFORM_TYPE_VEC4;
    }
    outComponents = 1;
    return WAYWAL_UNIFORM_TYPE_FLOAT;
}

void mergeShaderMetadata(const QString &src, waywal_shader_meta_t &outMeta, QString &outComboJson)
{
    waywal_shader_meta_init(&outMeta);
    const QByteArray utf8 = src.toUtf8();
    waywal_shader_parse_meta(utf8.constData(), static_cast<size_t>(utf8.size()), &outMeta);

    we_shader_meta_t weMeta{};
    we_shader_extract_metadata(utf8.constData(), &weMeta);

    for (uint32_t i = 0; i < weMeta.uniform_count; ++i) {
        const we_shader_uniform_meta_t &um = weMeta.uniforms[i];
        if (std::strcmp(um.type, "sampler2D") == 0 || um.name[0] == '\0') {
            continue;
        }
        if (waywal_shader_find_uniform(&outMeta, um.name) != nullptr) {
            continue;
        }
        if (outMeta.uniform_count >= WAYWAL_MAX_SHADER_UNIFORMS) {
            break;
        }

        waywal_shader_uniform_def_t &dst = outMeta.uniforms[outMeta.uniform_count++];
        std::memset(&dst, 0, sizeof(dst));
        std::snprintf(dst.name, sizeof(dst.name), "%s", um.name);
        std::snprintf(dst.label, sizeof(dst.label), "%s",
                      um.material_key[0] != '\0' ? um.material_key : um.name);
        uint32_t comps = 1;
        dst.type = mapWeTypeToWaywalType(um.type, um.name, comps);
        dst.num_components = um.num_components > 0 ? std::min(um.num_components, 4u) : comps;
        for (uint32_t c = 0; c < dst.num_components; ++c) {
            dst.default_val[c] = um.default_val[c];
            dst.min_val[c] = 0.0f;
            dst.max_val[c] = (um.default_val[c] > 1.0f) ? um.default_val[c] * 2.0f : 1.0f;
        }
        dst.step = (dst.type == WAYWAL_UNIFORM_TYPE_INT || dst.type == WAYWAL_UNIFORM_TYPE_BOOL)
                       ? 1.0f
                       : 0.01f;
    }

    QJsonArray comboArray;
    for (uint32_t i = 0; i < weMeta.combo_count; ++i) {
        QJsonObject obj;
        obj.insert(QStringLiteral("name"), QString::fromUtf8(weMeta.combos[i].name));
        obj.insert(QStringLiteral("value"), weMeta.combos[i].value);
        comboArray.append(obj);
    }
    outComboJson = QString::fromUtf8(QJsonDocument(comboArray).toJson(QJsonDocument::Compact));
}

QString computeShaderMetadataSignature(const waywal_shader_meta_t &meta,
                                       const QString &rawComboJson)
{
    QString sig;
    sig.reserve(512);
    sig += QString::fromUtf8(meta.title);
    sig += QLatin1Char('|');
    sig += QString::fromUtf8(meta.author);
    sig += QLatin1Char('|');
    sig += QString::fromUtf8(meta.description);
    sig += QLatin1Char('|');
    sig += QString::fromUtf8(meta.license);
    sig += QLatin1Char('|');
    sig += QString::number(meta.uniform_count);
    for (uint32_t i = 0; i < meta.uniform_count; ++i) {
        const auto &u = meta.uniforms[i];
        sig += QLatin1Char(';');
        sig += QString::fromUtf8(u.name);
        sig += QLatin1Char(':');
        sig += QString::fromUtf8(u.label);
        sig += QLatin1Char(':');
        sig += QString::number(static_cast<int>(u.type));
        sig += QLatin1Char(':');
        sig += QString::number(u.num_components);
        for (uint32_t c = 0; c < u.num_components; ++c) {
            sig += QLatin1Char(',');
            sig += QString::number(u.default_val[c], 'g', 7);
            sig += QLatin1Char(',');
            sig += QString::number(u.min_val[c], 'g', 7);
            sig += QLatin1Char(',');
            sig += QString::number(u.max_val[c], 'g', 7);
        }
        sig += QLatin1Char(':');
        sig += QString::number(u.step, 'g', 7);
        sig += QLatin1Char(':');
        sig += QString::number(u.has_range ? 1 : 0);
    }
    sig += QLatin1Char('|');
    sig += QString::number(meta.has_multi_pass ? 1 : 0);
    sig += QLatin1Char('|');
    sig += rawComboJson;
    return sig;
}

bool findJsonKeySpan(const QString &src, int jsonStart, int lineEnd, const QString &key,
                     int &outValStart, int &outValEnd)
{
    int p = jsonStart;
    int braceDepth = 0;
    while (p < lineEnd) {
        const QChar c = src[p];
        if (c == QLatin1Char('{')) {
            ++braceDepth;
            ++p;
            continue;
        }
        if (c == QLatin1Char('}')) {
            --braceDepth;
            if (braceDepth <= 0) {
                break;
            }
            ++p;
            continue;
        }
        if (c == QLatin1Char('"')) {
            const int strStart = p++;
            QString decoded;
            bool esc = false;
            while (p < lineEnd) {
                if (esc) {
                    decoded.append(src[p]);
                    esc = false;
                } else if (src[p] == QLatin1Char('\\')) {
                    esc = true;
                } else if (src[p] == QLatin1Char('"')) {
                    ++p;
                    break;
                } else {
                    decoded.append(src[p]);
                }
                ++p;
            }

            int afterQuote = p;
            while (afterQuote < lineEnd && src[afterQuote].isSpace()) {
                ++afterQuote;
            }
            if (braceDepth == 1 && afterQuote < lineEnd && src[afterQuote] == QLatin1Char(':') &&
                decoded == key) {
                p = afterQuote + 1;
                while (p < lineEnd && src[p].isSpace()) {
                    ++p;
                }
                if (p >= lineEnd) {
                    return false;
                }
                outValStart = p;
                if (src[p] == QLatin1Char('[')) {
                    int arrDepth = 1;
                    bool inArrStr = false;
                    bool arrEsc = false;
                    ++p;
                    while (p < lineEnd && arrDepth > 0) {
                        if (inArrStr) {
                            if (arrEsc) {
                                arrEsc = false;
                            } else if (src[p] == QLatin1Char('\\')) {
                                arrEsc = true;
                            } else if (src[p] == QLatin1Char('"')) {
                                inArrStr = false;
                            }
                        } else if (src[p] == QLatin1Char('"')) {
                            inArrStr = true;
                        } else if (src[p] == QLatin1Char('[')) {
                            ++arrDepth;
                        } else if (src[p] == QLatin1Char(']')) {
                            --arrDepth;
                        }
                        ++p;
                    }
                    outValEnd = p;
                    return arrDepth == 0;
                }
                if (src[p] == QLatin1Char('"')) {
                    ++p;
                    bool valEsc = false;
                    while (p < lineEnd) {
                        if (valEsc) {
                            valEsc = false;
                        } else if (src[p] == QLatin1Char('\\')) {
                            valEsc = true;
                        } else if (src[p] == QLatin1Char('"')) {
                            ++p;
                            break;
                        }
                        ++p;
                    }
                    outValEnd = p;
                    return outValEnd > outValStart;
                }
                while (p < lineEnd && !src[p].isSpace() && src[p] != QLatin1Char(',') &&
                       src[p] != QLatin1Char('}')) {
                    ++p;
                }
                outValEnd = p;
                return outValEnd > outValStart;
            }
            (void)strStart;
            continue;
        }
        ++p;
    }
    return false;
}

} // namespace

/* ========================================================================= */
/* GlslSceneScriptHighlighter                                                */
/* ========================================================================= */

GlslSceneScriptHighlighter::GlslSceneScriptHighlighter(QTextDocument *doc) : QSyntaxHighlighter(doc)
{
}

void GlslSceneScriptHighlighter::setLanguageMode(EditorLanguageMode mode)
{
    if (m_mode == mode) {
        return;
    }
    m_mode = mode;
    rehighlight();
}

struct GlslSceneScriptHighlighter::HighlightFormats {
    QTextCharFormat kwFmt;
    QTextCharFormat builtinFmt;
    QTextCharFormat pragmaFmt;
    QTextCharFormat strFmt;
    QTextCharFormat numFmt;
    QTextCharFormat commentFmt;

    HighlightFormats()
    {
        kwFmt.setForeground(QColor(QStringLiteral("#c084fc")));
        kwFmt.setFontWeight(QFont::Bold);
        builtinFmt.setForeground(QColor(QStringLiteral("#38bdf8")));
        pragmaFmt.setForeground(QColor(QStringLiteral("#f59e0b")));
        pragmaFmt.setFontWeight(QFont::Bold);
        strFmt.setForeground(QColor(QStringLiteral("#34d399")));
        numFmt.setForeground(QColor(QStringLiteral("#fb923c")));
        commentFmt.setForeground(QColor(QStringLiteral("#94a3b8")));
        commentFmt.setFontItalic(true);
    }
};

void GlslSceneScriptHighlighter::highlightCommentTag(const QString &text, int cStart, int &state,
                                                     const HighlightFormats &fmt)
{
    setFormat(cStart, text.size() - cStart, fmt.commentFmt);
    static const std::array<QLatin1StringView, 15> kTags = {{
        QLatin1StringView("[UNIFORM]"),
        QLatin1StringView("[COMBO]"),
        QLatin1StringView("[COMMON]"),
        QLatin1StringView("[BUFFER A]"),
        QLatin1StringView("[BUFFER B]"),
        QLatin1StringView("[BUFFER C]"),
        QLatin1StringView("[BUFFER D]"),
        QLatin1StringView("[IMAGE]"),
        QLatin1StringView("@uniform"),
        QLatin1StringView("@title"),
        QLatin1StringView("@author"),
        QLatin1StringView("@description"),
        QLatin1StringView("@license"),
        QLatin1StringView("@scale"),
        QLatin1StringView("@fps"),
    }};
    const QStringView commentView = QStringView(text).mid(cStart);
    for (const auto tag : kTags) {
        const int idx = commentView.indexOf(tag, 0, Qt::CaseInsensitive);
        if (idx >= 0) {
            setFormat(cStart + idx, tag.size(), fmt.pragmaFmt);
            if (tag == QLatin1StringView("[UNIFORM]")) {
                state |= kStateUniformBlock;
            }
        }
    }
    if (state & kStateUniformBlock) {
        static const std::array<QLatin1StringView, 7> kKeys = {{
            QLatin1StringView("type:"),
            QLatin1StringView("name:"),
            QLatin1StringView("label:"),
            QLatin1StringView("default:"),
            QLatin1StringView("min:"),
            QLatin1StringView("max:"),
            QLatin1StringView("step:"),
        }};
        for (const auto key : kKeys) {
            const int kidx = commentView.indexOf(key, 0, Qt::CaseInsensitive);
            if (kidx >= 0) {
                setFormat(cStart + kidx, key.size(), fmt.pragmaFmt);
            }
        }
    }
}

bool GlslSceneScriptHighlighter::consumeLeadingBlockComment(const QString &text, int &i, int &state,
                                                            const HighlightFormats &fmt)
{
    const int endIdx = text.indexOf(QLatin1String("*/"), 0);
    if (endIdx < 0) {
        setFormat(0, text.size(), fmt.commentFmt);
        setCurrentBlockState(state);
        return false;
    }
    setFormat(0, endIdx + 2, fmt.commentFmt);
    state &= ~kStateBlockComment;
    i = endIdx + 2;
    return true;
}

bool GlslSceneScriptHighlighter::consumeLeadingTemplateLiteral(const QString &text, int &i,
                                                               int &state,
                                                               const HighlightFormats &fmt)
{
    const int n = text.size();
    int j = i;
    bool closed = false;
    while (j < n) {
        if (text[j] == QLatin1Char('\\') && j + 1 < n) {
            j += 2;
            continue;
        }
        if (text[j] == QLatin1Char('`')) {
            ++j;
            closed = true;
            break;
        }
        ++j;
    }
    setFormat(i, j - i, fmt.strFmt);
    if (!closed) {
        setCurrentBlockState(state);
        return false;
    }
    state &= ~kStateTemplateLit;
    i = j;
    return true;
}

void GlslSceneScriptHighlighter::consumeLeadingDirective(const QString &text, int &i, int &state,
                                                         const HighlightFormats &fmt)
{
    const int n = text.size();
    int lead = 0;
    while (lead < n && text[lead].isSpace()) {
        ++lead;
    }
    if (lead + 1 < n && text[lead] == QLatin1Char('/') && text[lead + 1] == QLatin1Char('/')) {
        return;
    }
    if (i == 0 && lead < n && text[lead] == QLatin1Char('#') &&
        m_mode != EditorLanguageMode::SceneScriptJs) {
        state &= ~kStateUniformBlock;
        int endWord = lead + 1;
        while (endWord < n && isIdentChar(text[endWord])) {
            ++endWord;
        }
        setFormat(lead, endWord - lead, fmt.pragmaFmt);
        const int q1 = text.indexOf(QLatin1Char('"'), endWord);
        const int q2 = (q1 >= 0) ? text.indexOf(QLatin1Char('"'), q1 + 1) : -1;
        if (q1 >= 0 && q2 > q1) {
            setFormat(q1, q2 - q1 + 1, fmt.strFmt);
        }
        i = endWord;
        return;
    }
    state &= ~kStateUniformBlock;
}

int GlslSceneScriptHighlighter::scanQuotedToken(const QString &text, int i, int &state,
                                                const HighlightFormats &fmt, bool &closedOut)
{
    const QChar quote = text[i];
    const int n = text.size();
    int j = i + 1;
    closedOut = false;
    while (j < n) {
        if (text[j] == QLatin1Char('\\') && j + 1 < n) {
            j += 2;
            continue;
        }
        if (text[j] == quote) {
            ++j;
            closedOut = true;
            break;
        }
        ++j;
    }
    setFormat(i, j - i, fmt.strFmt);
    if (!closedOut && quote == QLatin1Char('`')) {
        state |= kStateTemplateLit;
    }
    return j;
}

int GlslSceneScriptHighlighter::scanNumberToken(const QString &text, int i,
                                                const HighlightFormats &fmt)
{
    const int n = text.size();
    int j = i;
    if (text[i] == QLatin1Char('0') && j + 1 < n &&
        (text[j + 1] == QLatin1Char('x') || text[j + 1] == QLatin1Char('X'))) {
        j += 2;
        while (j < n &&
               (text[j].isDigit() || (text[j] >= QLatin1Char('a') && text[j] <= QLatin1Char('f')) ||
                (text[j] >= QLatin1Char('A') && text[j] <= QLatin1Char('F')))) {
            ++j;
        }
    } else {
        while (j < n && (text[j].isDigit() || text[j] == QLatin1Char('.'))) {
            ++j;
        }
        if (j < n && (text[j] == QLatin1Char('e') || text[j] == QLatin1Char('E'))) {
            ++j;
            if (j < n && (text[j] == QLatin1Char('+') || text[j] == QLatin1Char('-'))) {
                ++j;
            }
            while (j < n && text[j].isDigit()) {
                ++j;
            }
        }
        if (j < n && (text[j] == QLatin1Char('f') || text[j] == QLatin1Char('F') ||
                      text[j] == QLatin1Char('u') || text[j] == QLatin1Char('U'))) {
            ++j;
        }
    }
    setFormat(i, j - i, fmt.numFmt);
    return j;
}

int GlslSceneScriptHighlighter::scanIdentifierToken(const QString &text, int i,
                                                    const HighlightFormats &fmt)
{
    const int n = text.size();
    int j = i + 1;
    while (j < n && isIdentChar(text[j])) {
        ++j;
    }
    const QStringView word = QStringView(text).mid(i, j - i);
    if (m_mode == EditorLanguageMode::SceneScriptJs) {
        if (matchesSortedWord(word, kJsKeywords)) {
            setFormat(i, j - i, fmt.kwFmt);
        } else if (matchesSortedWord(word, kJsBuiltins)) {
            setFormat(i, j - i, fmt.builtinFmt);
        }
    } else {
        if (matchesSortedWord(word, kGlslKeywords)) {
            setFormat(i, j - i, fmt.kwFmt);
        } else if (matchesSortedWord(word, kGlslUniformBuiltins)) {
            setFormat(i, j - i, fmt.pragmaFmt);
        } else if (matchesSortedWord(word, kGlslBuiltins)) {
            setFormat(i, j - i, fmt.builtinFmt);
        }
    }
    return j;
}

void GlslSceneScriptHighlighter::highlightBlock(const QString &text)
{
    static const HighlightFormats fmt;

    const int prev = previousBlockState();
    int state = (prev < 0) ? 0 : prev;
    const int n = text.size();
    int i = 0;

    if ((state & kStateBlockComment) && !consumeLeadingBlockComment(text, i, state, fmt)) {
        return;
    }
    if ((m_mode == EditorLanguageMode::SceneScriptJs) && (state & kStateTemplateLit) &&
        !consumeLeadingTemplateLiteral(text, i, state, fmt)) {
        return;
    }
    consumeLeadingDirective(text, i, state, fmt);

    while (i < n) {
        const QChar c = text[i];

        if (c == QLatin1Char('/') && i + 1 < n) {
            if (text[i + 1] == QLatin1Char('/')) {
                highlightCommentTag(text, i, state, fmt);
                setCurrentBlockState(state);
                return;
            }
            if (text[i + 1] == QLatin1Char('*')) {
                const int endIdx = text.indexOf(QLatin1String("*/"), i + 2);
                if (endIdx < 0) {
                    setFormat(i, n - i, fmt.commentFmt);
                    state |= kStateBlockComment;
                    setCurrentBlockState(state);
                    return;
                }
                setFormat(i, (endIdx + 2) - i, fmt.commentFmt);
                i = endIdx + 2;
                continue;
            }
        }

        if (c == QLatin1Char('"') || c == QLatin1Char('\'') ||
            (m_mode == EditorLanguageMode::SceneScriptJs && c == QLatin1Char('`'))) {
            bool closed = false;
            i = scanQuotedToken(text, i, state, fmt, closed);
            if (!closed && c == QLatin1Char('`')) {
                setCurrentBlockState(state);
                return;
            }
            continue;
        }

        if (c.isDigit() || (c == QLatin1Char('.') && i + 1 < n && text[i + 1].isDigit())) {
            i = scanNumberToken(text, i, fmt);
            continue;
        }

        if (isIdentStart(c)) {
            i = scanIdentifierToken(text, i, fmt);
            continue;
        }

        ++i;
    }

    setCurrentBlockState(state);
}

/* ========================================================================= */
/* CodeGutterWidget                                                          */
/* ========================================================================= */

CodeGutterWidget::CodeGutterWidget(QPlainTextEdit *editor) : QWidget(editor), m_editor(editor)
{
    setMouseTracking(true);
    setAccessibleName(QStringLiteral("Code line number and diagnostic gutter"));
    setAccessibleDescription(
        QStringLiteral("Displays line numbers and compiler error or warning indicators"));
}

QSize CodeGutterWidget::sizeHint() const
{
    int digits = 2;
    if (m_editor) {
        int maxLines = std::max(1, m_editor->blockCount());
        digits = 1;
        while (maxLines >= 10) {
            maxLines /= 10;
            ++digits;
        }
        digits = std::max(digits, 2);
    }
    const int charW = fontMetrics().horizontalAdvance(QLatin1Char('9'));
    return QSize(28 + digits * charW, 0);
}

void CodeGutterWidget::setDiagnostics(const std::vector<EditorDiagnostic> &diags)
{
    m_diagnostics = diags;
    update();
}

void CodeGutterWidget::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.fillRect(event->rect(), QColor(QStringLiteral("#12141a")));
    painter.setPen(QColor(QStringLiteral("#2a2f3d")));
    painter.drawLine(width() - 1, event->rect().top(), width() - 1, event->rect().bottom());

    auto *ed = dynamic_cast<EditorPlainTextEdit *>(m_editor);
    if (!ed) {
        return;
    }

    QTextBlock block = ed->publicFirstVisibleBlock();
    int blockNum = block.blockNumber();
    int top = static_cast<int>(std::round(
        ed->publicBlockBoundingGeometry(block).translated(ed->publicContentOffset()).top()));
    const int lineHeight = std::max(1, fontMetrics().height());
    int bottom =
        top + static_cast<int>(std::round(ed->publicBlockBoundingGeometry(block).height()));
    const int curBlock = ed->textCursor().blockNumber();

    painter.setFont(ed->font());

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            const uint32_t line1 = static_cast<uint32_t>(blockNum + 1);
            bool hasError = false;
            bool hasWarn = false;
            for (const auto &d : m_diagnostics) {
                if (d.line == line1) {
                    if (d.isWarning) {
                        hasWarn = true;
                    } else {
                        hasError = true;
                    }
                }
            }

            if (hasError || hasWarn) {
                painter.setRenderHint(QPainter::Antialiasing, true);
                painter.setPen(Qt::NoPen);
                painter.setBrush(hasError ? QColor(QStringLiteral("#ef4444"))
                                          : QColor(QStringLiteral("#f59e0b")));
                const int r = 4;
                const int cy = top + lineHeight / 2;
                painter.drawEllipse(QPoint(10, cy), r, r);
                painter.setRenderHint(QPainter::Antialiasing, false);
            }

            if (hasError) {
                painter.setPen(QColor(QStringLiteral("#ef4444")));
            } else if (hasWarn) {
                painter.setPen(QColor(QStringLiteral("#f59e0b")));
            } else if (blockNum == curBlock) {
                painter.setPen(QColor(QStringLiteral("#f1f5f9")));
            } else {
                painter.setPen(QColor(QStringLiteral("#64748b")));
            }
            painter.drawText(18, top, width() - 24, lineHeight, Qt::AlignRight | Qt::AlignVCenter,
                             QString::number(line1));
        }

        block = block.next();
        top = bottom;
        bottom =
            top + static_cast<int>(std::round(ed->publicBlockBoundingGeometry(block).height()));
        ++blockNum;
    }
}

void CodeGutterWidget::mouseMoveEvent(QMouseEvent *event)
{
    auto *ed = dynamic_cast<EditorPlainTextEdit *>(m_editor);
    if (!ed) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    QTextBlock block = ed->publicFirstVisibleBlock();
    int top = static_cast<int>(std::round(
        ed->publicBlockBoundingGeometry(block).translated(ed->publicContentOffset()).top()));
    int bottom =
        top + static_cast<int>(std::round(ed->publicBlockBoundingGeometry(block).height()));
    const int y = event->pos().y();

    while (block.isValid() && top <= y) {
        if (y >= top && y < bottom) {
            const uint32_t line1 = static_cast<uint32_t>(block.blockNumber() + 1);
            QStringList tips;
            for (const auto &d : m_diagnostics) {
                if (d.line == line1) {
                    tips.append(
                        QStringLiteral("L%1:%2 %3 %4")
                            .arg(d.line)
                            .arg(d.column)
                            .arg(d.isWarning ? QStringLiteral("[warn]") : QStringLiteral("[error]"),
                                 d.message));
                }
            }
            if (!tips.isEmpty()) {
                QToolTip::showText(event->globalPosition().toPoint(), tips.join(QLatin1Char('\n')),
                                   this);
            } else {
                QToolTip::hideText();
            }
            return;
        }
        block = block.next();
        top = bottom;
        bottom =
            top + static_cast<int>(std::round(ed->publicBlockBoundingGeometry(block).height()));
    }
    QToolTip::hideText();
}

void CodeGutterWidget::mousePressEvent(QMouseEvent *event)
{
    auto *ed = dynamic_cast<EditorPlainTextEdit *>(m_editor);
    if (!ed || event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    QTextBlock block = ed->publicFirstVisibleBlock();
    int top = static_cast<int>(std::round(
        ed->publicBlockBoundingGeometry(block).translated(ed->publicContentOffset()).top()));
    int bottom =
        top + static_cast<int>(std::round(ed->publicBlockBoundingGeometry(block).height()));
    const int y = event->pos().y();

    while (block.isValid() && top <= y) {
        if (y >= top && y < bottom) {
            const uint32_t line1 = static_cast<uint32_t>(block.blockNumber() + 1);
            uint32_t colOffset = 0;
            for (const auto &d : m_diagnostics) {
                if (d.line == line1 && d.column > 1) {
                    colOffset =
                        std::min<uint32_t>(d.column - 1, static_cast<uint32_t>(block.length() - 1));
                    break;
                }
            }
            QTextCursor cur(block);
            cur.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor,
                             static_cast<int>(colOffset));
            ed->setTextCursor(cur);
            ed->setFocus();
            event->accept();
            return;
        }
        block = block.next();
        top = bottom;
        bottom =
            top + static_cast<int>(std::round(ed->publicBlockBoundingGeometry(block).height()));
    }
    QWidget::mousePressEvent(event);
}

/* ========================================================================= */
/* GlslSceneScriptEditorWidget                                               */
/* ========================================================================= */

GlslSceneScriptEditorWidget::GlslSceneScriptEditorWidget(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("GlslSceneScriptEditorWidget"));
    setAccessibleName(QStringLiteral("GLSL ES 3.10 and SceneScript Code Editor"));
    setAccessibleDescription(
        QStringLiteral("Live-validated GLSL shader and SceneScript editor with diagnostic gutter"));
}

GlslSceneScriptEditorWidget::~GlslSceneScriptEditorWidget()
{
    if (m_ipcHotReloadConn) {
        disconnect(m_ipcHotReloadConn);
    }
    if (m_debounceTimer) {
        m_debounceTimer->stop();
    }
    stopValidationWorker();
    if (m_workerGlContext) {
        m_workerGlContext->doneCurrent();
    }
    m_workerGlContext.reset();
    m_workerSurface.reset();
}

void GlslSceneScriptEditorWidget::setIpcClient(IpcClient *client)
{
    if (m_ipcClient == client) {
        return;
    }
    if (m_ipcHotReloadConn) {
        disconnect(m_ipcHotReloadConn);
        m_ipcHotReloadConn = {};
    }
    m_ipcClient = client;
    if (m_ipcClient) {
        m_ipcHotReloadConn = connect(m_ipcClient, &IpcClient::codeHotReloaded, this,
                                     &GlslSceneScriptEditorWidget::onDaemonHotReloadResponded);
    }
}

void GlslSceneScriptEditorWidget::stopValidationWorker()
{
    if (auto *obj =
            findChild<QObject *>(QStringLiteral("ValidationMailbox"), Qt::FindDirectChildrenOnly)) {
        static_cast<ValidationMailbox *>(obj)->shutdown();
    }
    if (m_validationThread) {
        m_validationThread->wait();
        m_validationThread.reset();
    }
}

void GlslSceneScriptEditorWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    ensureUiInitialized();
    if (m_pendingShowValidation) {
        m_pendingShowValidation = false;
        triggerImmediateValidation();
    }
}

void GlslSceneScriptEditorWidget::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_F8) {
        jumpToDiagnostic((event->modifiers() & Qt::ShiftModifier) == 0);
        event->accept();
        return;
    }
    if ((event->modifiers() & Qt::ControlModifier) && event->key() == Qt::Key_F) {
        toggleFindBar();
        event->accept();
        return;
    }
    if ((event->modifiers() & Qt::ControlModifier) && event->key() == Qt::Key_S) {
        saveDocument();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape && m_findBar && m_findBar->isVisible()) {
        m_findBar->hide();
        if (m_codeEdit) {
            m_codeEdit->setFocus();
        }
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void GlslSceneScriptEditorWidget::ensureUiInitialized()
{
    if (m_uiInitialized) {
        return;
    }
    m_uiInitialized = true;

    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto *toolbar = new QWidget(this);
    toolbar->setObjectName(QStringLiteral("EditorToolbar"));
    toolbar->setStyleSheet(QStringLiteral(
        "#EditorToolbar { background: #12141a; border-bottom: 1px solid #2a2f3d; }"
        "QComboBox, QPushButton { background: #181b22; color: #f1f5f9; border: 1px solid #2a2f3d;"
        "  border-radius: 4px; padding: 3px 8px; font-size: 11px; }"
        "QComboBox:hover, QPushButton:hover { background: #222631; }"
        "QComboBox:focus, QPushButton:focus, QCheckBox:focus { border: 2px solid #38bdf8; }"
        "QCheckBox { color: #94a3b8; font-size: 11px; spacing: 4px; }"
        "QCheckBox::indicator { width: 13px; height: 13px; border-radius: 3px;"
        "  border: 1px solid #2a2f3d; background: #181b22; }"
        "QCheckBox::indicator:checked { background: #38bdf8; border-color: #38bdf8; }"));

    auto *tbLayout = new QHBoxLayout(toolbar);
    tbLayout->setContentsMargins(8, 4, 8, 4);
    tbLayout->setSpacing(8);

    m_languageCombo = new QComboBox(toolbar);
    m_languageCombo->addItem(QStringLiteral("GLSL ES 3.10 (Frag)"),
                             static_cast<int>(EditorLanguageMode::GlslEs310Fragment));
    m_languageCombo->addItem(QStringLiteral("GLSL ES 3.10 (Vert)"),
                             static_cast<int>(EditorLanguageMode::GlslEs310Vertex));
    m_languageCombo->addItem(QStringLiteral("SceneScript JS"),
                             static_cast<int>(EditorLanguageMode::SceneScriptJs));
    m_languageCombo->setCurrentIndex(static_cast<int>(m_languageMode));
    m_languageCombo->setAccessibleName(QStringLiteral("Editor language mode"));
    m_languageCombo->setAccessibleDescription(
        QStringLiteral("Switch between GLSL ES 3.10 fragment, vertex, and SceneScript JavaScript"));
    tbLayout->addWidget(m_languageCombo);

    m_hotReloadCheck = new QCheckBox(QStringLiteral("Live Hot-Reload"), toolbar);
    m_hotReloadCheck->setChecked(m_liveHotReload);
    m_hotReloadCheck->setAccessibleName(QStringLiteral("Live hot reload toggle"));
    tbLayout->addWidget(m_hotReloadCheck);

    m_syncDefaultsCheck = new QCheckBox(QStringLiteral("Sync Defaults"), toolbar);
    m_syncDefaultsCheck->setChecked(m_syncDefaultsToCode);
    m_syncDefaultsCheck->setAccessibleName(QStringLiteral("Sync inspector defaults to code"));
    tbLayout->addWidget(m_syncDefaultsCheck);

    tbLayout->addStretch(1);

    m_nextDiagBtn = new QPushButton(QStringLiteral("Next Err (F8)"), toolbar);
    m_nextDiagBtn->setAccessibleName(QStringLiteral("Jump to next diagnostic"));
    tbLayout->addWidget(m_nextDiagBtn);

    m_findBtn = new QPushButton(QStringLiteral("Find"), toolbar);
    m_findBtn->setAccessibleName(QStringLiteral("Find text in code"));
    tbLayout->addWidget(m_findBtn);

    m_saveBtn = new QPushButton(QStringLiteral("Save"), toolbar);
    m_saveBtn->setAccessibleName(QStringLiteral("Save snapshot and apply"));
    tbLayout->addWidget(m_saveBtn);

    m_revertBtn = new QPushButton(QStringLiteral("Revert"), toolbar);
    m_revertBtn->setAccessibleName(QStringLiteral("Revert to last saved snapshot"));
    tbLayout->addWidget(m_revertBtn);

    m_validateBtn = new QPushButton(QStringLiteral("Validate"), toolbar);
    m_validateBtn->setAccessibleName(QStringLiteral("Validate code immediately"));
    tbLayout->addWidget(m_validateBtn);

    m_statusPill = new QLabel(QStringLiteral("Ready"), toolbar);
    m_statusPill->setAccessibleName(QStringLiteral("Validation status"));
    m_statusPill->setStyleSheet(QStringLiteral(
        "background: #181b22; color: #94a3b8; border: 1px solid #2a2f3d;"
        "border-radius: 10px; padding: 2px 8px; font-size: 11px; font-weight: 600;"));
    tbLayout->addWidget(m_statusPill);

    rootLayout->addWidget(toolbar);

    m_findBar = new QWidget(this);
    m_findBar->setObjectName(QStringLiteral("EditorFindBar"));
    m_findBar->setStyleSheet(QStringLiteral(
        "#EditorFindBar { background: #12141a; border-bottom: 1px solid #2a2f3d; }"
        "QLineEdit { background: #181b22; color: #f1f5f9; border: 1px solid #2a2f3d; "
        "border-radius: 4px; padding: 2px 6px; font-size: 11px; }"
        "QPushButton { background: #181b22; color: #f1f5f9; border: 1px solid #2a2f3d; "
        "border-radius: 4px; padding: 2px 8px; font-size: 11px; }"));
    auto *findLayout = new QHBoxLayout(m_findBar);
    findLayout->setContentsMargins(8, 3, 8, 3);
    findLayout->setSpacing(6);
    m_findEdit = new QLineEdit(m_findBar);
    m_findEdit->setPlaceholderText(QStringLiteral("Find in source..."));
    auto *findPrevBtn = new QPushButton(QStringLiteral("Prev"), m_findBar);
    auto *findNextBtn = new QPushButton(QStringLiteral("Next"), m_findBar);
    auto *findCloseBtn = new QPushButton(QStringLiteral("Close"), m_findBar);
    findLayout->addWidget(m_findEdit, 1);
    findLayout->addWidget(findPrevBtn);
    findLayout->addWidget(findNextBtn);
    findLayout->addWidget(findCloseBtn);
    m_findBar->hide();
    rootLayout->addWidget(m_findBar);

    auto *editor = new EditorPlainTextEdit(this);
    m_codeEdit = editor;
    m_codeEdit->setObjectName(QStringLiteral("GlslSceneScriptCodeEdit"));
    m_codeEdit->setAccessibleName(QStringLiteral("Shader and script source editor"));
    m_codeEdit->setAccessibleDescription(
        QStringLiteral("Monospace code editor with line numbers and inline diagnostics"));
    m_codeEdit->setLineWrapMode(QPlainTextEdit::NoWrap);

    QFont monoFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    monoFont.setFamilies({QStringLiteral("JetBrains Mono"), QStringLiteral("monospace")});
    monoFont.setPointSize(10);
    monoFont.setStyleHint(QFont::Monospace);
    m_codeEdit->setFont(monoFont);
    m_codeEdit->setTabStopDistance(QFontMetricsF(monoFont).horizontalAdvance(QLatin1Char(' ')) *
                                   4.0);
    m_codeEdit->setStyleSheet(
        QStringLiteral("QPlainTextEdit { background: #0b0c10; color: #f1f5f9; border: none;"
                       "  selection-background-color: #222631; selection-color: #f1f5f9; }"));

    m_highlighter = new GlslSceneScriptHighlighter(m_codeEdit->document());
    m_highlighter->setLanguageMode(m_languageMode);

    m_gutter = new CodeGutterWidget(m_codeEdit);
    m_gutter->setFont(monoFont);
    editor->setGutterWidget(m_gutter);
    editor->updateGutterMargins();

    rootLayout->addWidget(m_codeEdit, 1);

    m_debounceTimer = new QTimer(this);
    m_debounceTimer->setSingleShot(true);
    m_debounceTimer->setInterval(kDebounceValidationMs);

    connect(m_debounceTimer, &QTimer::timeout, this,
            &GlslSceneScriptEditorWidget::runDebouncedValidation);
    connect(m_codeEdit, &QPlainTextEdit::textChanged, this,
            &GlslSceneScriptEditorWidget::onEditorTextChanged);
    connect(m_codeEdit, &QPlainTextEdit::blockCountChanged, this, [this, editor](int) {
        editor->updateGutterMargins();
        if (m_gutter) {
            m_gutter->update();
        }
    });
    connect(m_codeEdit, &QPlainTextEdit::updateRequest, this, [this](const QRect &rect, int dy) {
        if (!m_gutter) {
            return;
        }
        if (dy != 0) {
            m_gutter->scroll(0, dy);
        } else {
            m_gutter->update(0, rect.y(), m_gutter->width(), rect.height());
        }
        if (rect.contains(m_codeEdit->viewport()->rect())) {
            if (auto *ed = dynamic_cast<EditorPlainTextEdit *>(m_codeEdit)) {
                ed->updateGutterMargins();
            }
        }
    });
    connect(m_codeEdit, &QPlainTextEdit::cursorPositionChanged, this, [this]() {
        if (m_gutter) {
            m_gutter->update();
        }
    });

    connect(m_languageCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int idx) {
                if (idx < 0 || !m_languageCombo) {
                    return;
                }
                m_languageMode =
                    static_cast<EditorLanguageMode>(m_languageCombo->itemData(idx).toInt());
                if (m_highlighter) {
                    m_highlighter->setLanguageMode(m_languageMode);
                }
                triggerImmediateValidation();
            });
    connect(m_hotReloadCheck, &QCheckBox::toggled, this,
            [this](bool checked) { m_liveHotReload = checked; });
    connect(m_syncDefaultsCheck, &QCheckBox::toggled, this,
            [this](bool checked) { m_syncDefaultsToCode = checked; });
    connect(m_validateBtn, &QPushButton::clicked, this,
            &GlslSceneScriptEditorWidget::triggerImmediateValidation);
    connect(m_nextDiagBtn, &QPushButton::clicked, this, [this]() { jumpToDiagnostic(true); });
    connect(m_findBtn, &QPushButton::clicked, this, &GlslSceneScriptEditorWidget::toggleFindBar);
    connect(m_saveBtn, &QPushButton::clicked, this, &GlslSceneScriptEditorWidget::saveDocument);
    connect(m_revertBtn, &QPushButton::clicked, this, &GlslSceneScriptEditorWidget::revertDocument);
    connect(m_findEdit, &QLineEdit::returnPressed, this, [this]() { findNextInEditor(true); });
    connect(findNextBtn, &QPushButton::clicked, this, [this]() { findNextInEditor(true); });
    connect(findPrevBtn, &QPushButton::clicked, this, [this]() { findNextInEditor(false); });
    connect(findCloseBtn, &QPushButton::clicked, this, [this]() {
        if (m_findBar) {
            m_findBar->hide();
        }
        if (m_codeEdit) {
            m_codeEdit->setFocus();
        }
    });
}

void GlslSceneScriptEditorWidget::ensureValidationContext()
{
    if (m_workerGlContext && m_workerSurface) {
        return;
    }
    if (!QGuiApplication::instance()) {
        return;
    }

    auto surface = std::make_unique<QOffscreenSurface>();
    QSurfaceFormat fmt;
    if (m_shareContext && m_shareContext->isValid()) {
        fmt = m_shareContext->format();
    } else {
        fmt.setRenderableType(QSurfaceFormat::OpenGLES);
        fmt.setVersion(3, 1);
    }
    surface->setFormat(fmt);
    surface->create();
    if (!surface->isValid()) {
        return;
    }

    auto ctx = std::make_unique<QOpenGLContext>();
    ctx->setFormat(fmt);
    if (m_shareContext && m_shareContext->isValid()) {
        ctx->setShareContext(m_shareContext);
    }
    if (!ctx->create() && m_shareContext) {
        // Retry unshared if the platform QPA refuses cross-context sharing offscreen.
        ctx->setShareContext(nullptr);
        ctx->create();
    }
    if (!ctx->isValid()) {
        return;
    }

    m_workerSurface = std::move(surface);
    m_workerGlContext = std::move(ctx);
}

void GlslSceneScriptEditorWidget::openDocument(EditorLanguageMode mode, int32_t objectId,
                                               uint32_t passOrScriptSlot, const QString &title,
                                               const QString &sourceText)
{
    ensureUiInitialized();
    m_languageMode = mode;
    m_objectId = objectId;
    m_slotIndex = passOrScriptSlot;
    m_lastEmittedMetaSig.clear();
    m_savedSourceText = sourceText;
    m_sourceSpansDirty = true;
    m_lastDiagJumpIdx = -1;

    if (m_languageCombo) {
        const QSignalBlocker blocker(m_languageCombo);
        m_languageCombo->setCurrentIndex(static_cast<int>(mode));
    }
    if (m_highlighter) {
        m_highlighter->setLanguageMode(mode);
    }

    {
        QScopedValueRollback<bool> guard(m_syncingFromInspector, true);
        m_codeEdit->setPlainText(sourceText);
    }
    if (auto *ed = dynamic_cast<EditorPlainTextEdit *>(m_codeEdit)) {
        ed->updateGutterMargins();
    }

    if (!title.isEmpty()) {
        emit statusMessage(tr("Opened %1").arg(title));
    }
    if (isVisible()) {
        triggerImmediateValidation();
    } else {
        m_pendingShowValidation = true;
    }
}

QString GlslSceneScriptEditorWidget::sourceText() const
{
    return m_codeEdit ? m_codeEdit->toPlainText() : QString();
}

void GlslSceneScriptEditorWidget::onEditorTextChanged()
{
    if (m_syncingFromInspector) {
        return;
    }
    m_sourceSpansDirty = true;
    m_editGeneration.fetch_add(1, std::memory_order_acq_rel);
    if (m_statusPill) {
        m_statusPill->setText(QStringLiteral("Editing..."));
        m_statusPill->setStyleSheet(QStringLiteral(
            "background: #181b22; color: #94a3b8; border: 1px solid #2a2f3d;"
            "border-radius: 10px; padding: 2px 8px; font-size: 11px; font-weight: 600;"));
    }
    if (m_debounceTimer) {
        m_debounceTimer->start(kDebounceValidationMs);
    }
}

void GlslSceneScriptEditorWidget::triggerImmediateValidation()
{
    ensureUiInitialized();
    m_pendingShowValidation = false;
    if (m_debounceTimer) {
        m_debounceTimer->stop();
    }
    m_editGeneration.fetch_add(1, std::memory_order_acq_rel);
    runDebouncedValidation();
}

void GlslSceneScriptEditorWidget::jumpToDiagnostic(bool forward)
{
    if (m_diagnostics.empty() || !m_codeEdit) {
        return;
    }
    const int n = static_cast<int>(m_diagnostics.size());
    if (forward) {
        m_lastDiagJumpIdx = (m_lastDiagJumpIdx + 1) % n;
    } else {
        m_lastDiagJumpIdx = (m_lastDiagJumpIdx <= 0) ? (n - 1) : (m_lastDiagJumpIdx - 1);
    }
    const EditorDiagnostic &diag = m_diagnostics[static_cast<size_t>(m_lastDiagJumpIdx)];
    QTextDocument *doc = m_codeEdit->document();
    const QTextBlock block =
        doc->findBlockByNumber(static_cast<int>(diag.line > 0 ? diag.line - 1 : 0));
    if (block.isValid()) {
        QTextCursor cur(block);
        if (diag.column > 1) {
            const int colOff =
                std::min<int>(static_cast<int>(diag.column - 1), std::max(0, block.length() - 1));
            cur.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, colOff);
        }
        m_codeEdit->setTextCursor(cur);
        m_codeEdit->centerCursor();
        m_codeEdit->setFocus();
    }
    emit statusMessage(
        QStringLiteral("L%1:%2 %3").arg(diag.line).arg(diag.column).arg(diag.message));
}

void GlslSceneScriptEditorWidget::toggleFindBar()
{
    ensureUiInitialized();
    if (!m_findBar || !m_findEdit) {
        return;
    }
    if (m_findBar->isVisible()) {
        m_findBar->hide();
        if (m_codeEdit) {
            m_codeEdit->setFocus();
        }
        return;
    }
    m_findBar->show();
    m_findEdit->setFocus();
    m_findEdit->selectAll();
}

void GlslSceneScriptEditorWidget::findNextInEditor(bool forward)
{
    if (!m_codeEdit || !m_findEdit) {
        return;
    }
    const QString needle = m_findEdit->text();
    if (needle.isEmpty()) {
        return;
    }
    QTextDocument::FindFlags flags;
    if (!forward) {
        flags |= QTextDocument::FindBackward;
    }
    if (!m_codeEdit->find(needle, flags)) {
        QTextCursor wrapCur = m_codeEdit->textCursor();
        wrapCur.movePosition(forward ? QTextCursor::Start : QTextCursor::End);
        m_codeEdit->setTextCursor(wrapCur);
        (void)m_codeEdit->find(needle, flags);
    }
}

void GlslSceneScriptEditorWidget::saveDocument()
{
    if (!m_codeEdit) {
        return;
    }
    m_savedSourceText = m_codeEdit->toPlainText();
    emit statusMessage(tr("Saved snapshot (%1 bytes)").arg(m_savedSourceText.size()));
    triggerImmediateValidation();
}

void GlslSceneScriptEditorWidget::revertDocument()
{
    if (!m_codeEdit) {
        return;
    }
    {
        QScopedValueRollback<bool> guard(m_syncingFromInspector, true);
        m_codeEdit->setPlainText(m_savedSourceText);
    }
    m_sourceSpansDirty = true;
    emit statusMessage(tr("Reverted to last saved snapshot"));
    triggerImmediateValidation();
}

void GlslSceneScriptEditorWidget::runDebouncedValidation()
{
    if (!m_codeEdit) {
        return;
    }
    const uint64_t generation = m_editGeneration.load(std::memory_order_acquire);
    const QString src = m_codeEdit->toPlainText();
    const EditorLanguageMode mode = m_languageMode;

    if (m_sourceSpansDirty) {
        rebuildSourceSpans(src);
    }

    auto *mailboxObj =
        findChild<QObject *>(QStringLiteral("ValidationMailbox"), Qt::FindDirectChildrenOnly);
    auto *mailbox =
        mailboxObj ? static_cast<ValidationMailbox *>(mailboxObj) : new ValidationMailbox(this);

    if (!m_validationThread) {
        QPointer<GlslSceneScriptEditorWidget> self(this);
        m_validationThread.reset(QThread::create([self, mailbox]() {
            ValidationMailbox::Job job;
            while (mailbox->waitNext(job)) {
                if (!self) {
                    break;
                }
                if (job.generation != self->m_editGeneration.load(std::memory_order_acquire)) {
                    continue;
                }

                const auto t0 = std::chrono::steady_clock::now();
                std::vector<EditorDiagnostic> diags;
                QByteArray preprocessedGlsl;
                uint32_t prepComboCount = 0;
                const uint32_t maxLines = static_cast<uint32_t>(
                    std::max<qsizetype>(1, job.source.count(QLatin1Char('\n')) + 1));

                const QByteArray utf8Source = job.source.toUtf8();
                if (job.mode == EditorLanguageMode::SceneScriptJs) {
                    we_script_diag_t diag{};
                    if (!we_script_lint(utf8Source.constData(),
                                        static_cast<size_t>(utf8Source.size()), 0u, &diag)) {
                        EditorDiagnostic ed;
                        ed.line = std::clamp(diag.error_line, 1u, maxLines);
                        ed.column = std::max(1u, diag.error_col);
                        ed.isWarning = false;
                        ed.message = QString::fromUtf8(diag.error_msg);
                        if (ed.message.isEmpty()) {
                            ed.message = QStringLiteral("SceneScript syntax error");
                        }
                        diags.push_back(std::move(ed));
                    }
                } else {
                    const bool isFrag = (job.mode == EditorLanguageMode::GlslEs310Fragment);
                    const bool nativeEs = isStandaloneNativeGles(job.source);
                    if (nativeEs) {
                        preprocessedGlsl = utf8Source;
                    } else {
                        we_shader_meta_t prepMeta{};
                        char *preprocessed =
                            we_shader_preprocess_stage(utf8Source.constData(), isFrag, nullptr,
                                                       nullptr, 0, 0, false, &prepMeta);
                        prepComboCount = prepMeta.combo_count;
                        if (preprocessed) {
                            preprocessedGlsl = QByteArray(preprocessed);
                            std::free(preprocessed);
                        } else {
                            diags.push_back(EditorDiagnostic{
                                1u, 1u, false,
                                QStringLiteral(
                                    "Shader preprocessor failed (#include limit or OOM)")});
                        }
                    }

                    if (diags.empty()) {
                        diags = validateGlslStructuralFallback(job.source);
                    }
                }

                const auto t1 = std::chrono::steady_clock::now();
                const uint32_t elapsedUs = static_cast<uint32_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());

                const uint64_t doneGen = job.generation;
                const EditorLanguageMode doneMode = job.mode;
                if (!self) {
                    break;
                }
                QMetaObject::invokeMethod(
                    self.data(),
                    [self, doneGen, doneMode, srcBytes = std::move(utf8Source),
                     d = std::move(diags), prepGlsl = std::move(preprocessedGlsl), prepComboCount,
                     maxLines, elapsedUs]() mutable {
                        if (self) {
                            self->finishValidationOnMainThread(
                                doneGen, doneMode, std::move(srcBytes), std::move(d),
                                std::move(prepGlsl), prepComboCount, maxLines, elapsedUs);
                        }
                    },
                    Qt::QueuedConnection);
            }
        }));
        m_validationThread->start();
    }
    mailbox->submit(generation, mode, src);
}

void GlslSceneScriptEditorWidget::finishValidationOnMainThread(
    uint64_t doneGen, EditorLanguageMode doneMode, QByteArray srcBytes,
    std::vector<EditorDiagnostic> diags, QByteArray preprocessedGlsl, uint32_t prepComboCount,
    uint32_t maxLines, uint32_t elapsedUs)
{
    if (doneGen != m_editGeneration.load(std::memory_order_acquire)) {
        return;
    }

    const QString srcStr = QString::fromUtf8(srcBytes);

    // Run GLES shader compilation strictly on the GUI thread where QOffscreenSurface affinity
    // and QOpenGLContext ownership live, avoiding cross-thread QOffscreenSurface races.
    if (doneMode != EditorLanguageMode::SceneScriptJs && diags.empty() &&
        !preprocessedGlsl.isEmpty()) {
        ensureValidationContext();
        if (m_workerGlContext && m_workerGlContext->isValid() && m_workerSurface &&
            m_workerSurface->isValid() && m_workerGlContext->makeCurrent(m_workerSurface.get())) {
            const auto tGl0 = std::chrono::steady_clock::now();
            QOpenGLFunctions *gl = m_workerGlContext->functions();
            if (gl) {
                const bool isFrag = (doneMode == EditorLanguageMode::GlslEs310Fragment);
                const GLenum stage = isFrag ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER;
                const GLuint shader = gl->glCreateShader(stage);
                if (shader != 0) {
                    const char *srcPtr = preprocessedGlsl.constData();
                    const GLint srcLen = static_cast<GLint>(preprocessedGlsl.size());
                    gl->glShaderSource(shader, 1, &srcPtr, &srcLen);
                    gl->glCompileShader(shader);

                    GLint compiled = GL_FALSE;
                    gl->glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
                    GLint logLen = 0;
                    gl->glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLen);

                    QString logText;
                    if (logLen > 1) {
                        QByteArray logBuf(logLen, '\0');
                        GLsizei written = 0;
                        gl->glGetShaderInfoLog(shader, logLen, &written, logBuf.data());
                        logText = QString::fromUtf8(logBuf.constData(), written);
                    }
                    gl->glDeleteShader(shader);

                    if (!logText.isEmpty()) {
                        const bool nativeEs = isStandaloneNativeGles(srcStr);
                        const std::vector<LineMapEntry> lineMap =
                            nativeEs ? std::vector<LineMapEntry>{}
                                     : buildPreprocessedLineMap(srcStr, preprocessedGlsl, isFrag,
                                                                prepComboCount);
                        diags = parseGlslInfoLog(logText, lineMap, maxLines);
                    }
                    if (compiled == GL_FALSE && diags.empty()) {
                        diags.push_back(EditorDiagnostic{
                            1u, 1u, false,
                            logText.isEmpty() ? QStringLiteral("GLSL compilation failed")
                                              : logText.trimmed()});
                    }
                }
            }
            m_workerGlContext->doneCurrent();
            const auto tGl1 = std::chrono::steady_clock::now();
            elapsedUs += static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(tGl1 - tGl0).count());
        }
    }

    applyValidationResult(doneGen, std::move(diags));
    const bool hasErr =
        std::ranges::any_of(m_diagnostics, [](const EditorDiagnostic &x) { return !x.isWarning; });

    if (!hasErr && doneMode != EditorLanguageMode::SceneScriptJs) {
        waywal_shader_meta_t meta{};
        QString rawComboJson;
        mergeShaderMetadata(srcStr, meta, rawComboJson);
        const QString sig = computeShaderMetadataSignature(meta, rawComboJson);
        if (sig != m_lastEmittedMetaSig) {
            m_lastEmittedMetaSig = sig;
            emit shaderMetadataExtracted(meta, rawComboJson);
        }
    }

    if (m_liveHotReload) {
        const bool ipcLive = (m_ipcClient && m_ipcClient->isConnected());
        if (!hasErr && m_ipcClient) {
            sendHotReloadCodePacket(m_ipcClient, m_targetOutput, doneMode, m_objectId, m_slotIndex,
                                    srcBytes);
        }
        if (!ipcLive || hasErr) {
            emit liveHotReloadApplied(!hasErr, elapsedUs);
        }
    }
}

void GlslSceneScriptEditorWidget::onDaemonHotReloadResponded(
    const waywal_ipc_resp_hot_reload_code_t &resp, const QString &metaJson)
{
    (void)metaJson;
    const bool ok = (resp.status_code == 0u);
    if (!ok && resp.error_msg[0] != '\0') {
        const uint32_t maxLines =
            m_codeEdit ? static_cast<uint32_t>(std::max(1, m_codeEdit->blockCount())) : 1u;
        EditorDiagnostic d;
        d.line = std::clamp(std::max(1u, resp.error_line), 1u, maxLines);
        d.column = std::max(1u, resp.error_col);
        d.isWarning = false;
        d.message = QString::fromUtf8(resp.error_msg);
        applyValidationResult(m_editGeneration.load(std::memory_order_acquire), {std::move(d)});
    }
    emit liveHotReloadApplied(ok, resp.compile_time_us);
}

void GlslSceneScriptEditorWidget::applyValidationResult(uint64_t generation,
                                                        std::vector<EditorDiagnostic> diags)
{
    if (generation != m_editGeneration.load(std::memory_order_acquire)) {
        return;
    }

    m_diagnostics = std::move(diags);
    if (m_gutter) {
        m_gutter->setDiagnostics(m_diagnostics);
    }

    if (m_codeEdit) {
        QList<QTextEdit::ExtraSelection> selections;
        QTextDocument *doc = m_codeEdit->document();
        for (const auto &diag : m_diagnostics) {
            const int blkIdx = static_cast<int>(diag.line > 0 ? diag.line - 1 : 0);
            const QTextBlock block = doc->findBlockByNumber(blkIdx);
            if (!block.isValid()) {
                continue;
            }

            QTextEdit::ExtraSelection sel;
            sel.format.setBackground(diag.isWarning ? QColor(245, 158, 11, 28)
                                                    : QColor(239, 68, 68, 36));
            sel.format.setUnderlineStyle(QTextCharFormat::WaveUnderline);
            sel.format.setUnderlineColor(diag.isWarning ? QColor(QStringLiteral("#f59e0b"))
                                                        : QColor(QStringLiteral("#ef4444")));
            sel.format.setProperty(QTextFormat::FullWidthSelection, true);

            QTextCursor cur(block);
            if (diag.column > 1 && static_cast<int>(diag.column - 1) < block.length()) {
                cur.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor,
                                 static_cast<int>(diag.column - 1));
                cur.movePosition(QTextCursor::EndOfWord, QTextCursor::KeepAnchor);
                if (!cur.hasSelection()) {
                    cur.select(QTextCursor::LineUnderCursor);
                }
            } else {
                cur.select(QTextCursor::LineUnderCursor);
            }
            sel.cursor = cur;
            selections.append(sel);
        }
        m_codeEdit->setExtraSelections(selections);
    }

    size_t errCount = 0;
    size_t warnCount = 0;
    for (const auto &d : m_diagnostics) {
        if (d.isWarning) {
            ++warnCount;
        } else {
            ++errCount;
        }
    }

    if (m_statusPill) {
        if (errCount > 0) {
            m_statusPill->setText(QStringLiteral("%1 Err").arg(errCount));
            m_statusPill->setStyleSheet(QStringLiteral(
                "background: #181b22; color: #ef4444; border: 1px solid #ef4444;"
                "border-radius: 10px; padding: 2px 8px; font-size: 11px; font-weight: 600;"));
        } else if (warnCount > 0) {
            m_statusPill->setText(QStringLiteral("%1 Warn").arg(warnCount));
            m_statusPill->setStyleSheet(QStringLiteral(
                "background: #181b22; color: #f59e0b; border: 1px solid #f59e0b;"
                "border-radius: 10px; padding: 2px 8px; font-size: 11px; font-weight: 600;"));
        } else {
            m_statusPill->setText(QStringLiteral("OK"));
            m_statusPill->setStyleSheet(QStringLiteral(
                "background: #181b22; color: #22c55e; border: 1px solid #22c55e;"
                "border-radius: 10px; padding: 2px 8px; font-size: 11px; font-weight: 600;"));
        }
    }

    emit diagnosticsUpdated(m_diagnostics);
}

void GlslSceneScriptEditorWidget::rebuildSourceSpans(const QString &src)
{
    m_sourceSpans.clear();
    if (src.isEmpty()) {
        return;
    }

    bool inUniformBlock = false;
    QString blockName;
    int blockDefStart = -1;
    int blockDefEnd = -1;

    auto flushUniformBlock = [&]() {
        if (!blockName.isEmpty() && blockDefStart >= 0 && blockDefEnd >= blockDefStart) {
            m_sourceSpans.push_back(
                ShaderSourceSpan{blockName, QStringLiteral("default"), blockDefStart, blockDefEnd});
        }
        blockName.clear();
        blockDefStart = -1;
        blockDefEnd = -1;
    };

    const int total = src.size();
    int lineStart = 0;

    while (lineStart < total) {
        int nl = src.indexOf(QLatin1Char('\n'), lineStart);
        const int lineEnd = (nl >= 0) ? nl : total;
        int contentEnd = lineEnd;
        if (contentEnd > lineStart && src[contentEnd - 1] == QLatin1Char('\r')) {
            --contentEnd;
        }

        int s = lineStart;
        while (s < contentEnd && src[s].isSpace()) {
            ++s;
        }
        int e = contentEnd;
        while (e > s && src[e - 1].isSpace()) {
            --e;
        }

        if (e - s >= 2 && src[s] == QLatin1Char('/') && src[s + 1] == QLatin1Char('/')) {
            int c = s + 2;
            while (c < e && src[c].isSpace()) {
                ++c;
            }
            const QStringView comment = QStringView(src).mid(c, e - c);

            if (comment.startsWith(QLatin1String("[UNIFORM]"), Qt::CaseInsensitive)) {
                flushUniformBlock();
                inUniformBlock = true;
            } else if (comment.startsWith(QLatin1String("[COMBO]"), Qt::CaseInsensitive)) {
                flushUniformBlock();
                inUniformBlock = false;
                const int bracePos = src.indexOf(QLatin1Char('{'), c);
                if (bracePos >= 0 && bracePos < e) {
                    int nameStart = -1;
                    int nameEnd = -1;
                    if (!findJsonKeySpan(src, bracePos, e, QStringLiteral("combo"), nameStart,
                                         nameEnd)) {
                        findJsonKeySpan(src, bracePos, e, QStringLiteral("name"), nameStart,
                                        nameEnd);
                    }
                    int defStart = -1;
                    int defEnd = -1;
                    if (nameStart >= 0 && nameEnd > nameStart + 2 &&
                        findJsonKeySpan(src, bracePos, e, QStringLiteral("default"), defStart,
                                        defEnd)) {
                        const QString cname = src.mid(nameStart + 1, nameEnd - nameStart - 2);
                        m_sourceSpans.push_back(ShaderSourceSpan{
                            cname, QStringLiteral("combo_default"), defStart, defEnd});
                    }
                }
            } else if (comment.startsWith(QLatin1String("@uniform"), Qt::CaseInsensitive)) {
                flushUniformBlock();
                inUniformBlock = false;
                int p = c + 8;
                while (p < e && src[p].isSpace()) {
                    ++p;
                }
                while (p < e && !src[p].isSpace()) {
                    ++p;
                }
                while (p < e && src[p].isSpace()) {
                    ++p;
                }
                const int nameStart = p;
                while (p < e && isIdentChar(src[p])) {
                    ++p;
                }
                const QString uname = src.mid(nameStart, p - nameStart);
                while (p < e && src[p].isSpace()) {
                    ++p;
                }
                if (p < e && src[p] == QLatin1Char('"')) {
                    ++p;
                    while (p < e && src[p] != QLatin1Char('"')) {
                        ++p;
                    }
                    if (p < e) {
                        ++p;
                    }
                }
                const int eqPos = src.indexOf(QLatin1Char('='), p);
                if (!uname.isEmpty() && eqPos >= 0 && eqPos < e) {
                    int vStart = eqPos + 1;
                    while (vStart < e && src[vStart].isSpace()) {
                        ++vStart;
                    }
                    int vEnd = vStart;
                    while (vEnd < e && src[vEnd] != QLatin1Char(';') &&
                           src[vEnd] != QLatin1Char('[')) {
                        if (src[vEnd] == QLatin1Char('/') && vEnd + 1 < e &&
                            src[vEnd + 1] == QLatin1Char('/')) {
                            break;
                        }
                        ++vEnd;
                    }
                    while (vEnd > vStart && src[vEnd - 1].isSpace()) {
                        --vEnd;
                    }
                    if (vEnd > vStart) {
                        m_sourceSpans.push_back(
                            ShaderSourceSpan{uname, QStringLiteral("default"), vStart, vEnd});
                    }
                }
            } else if (inUniformBlock) {
                if (comment.startsWith(QLatin1String("name:"), Qt::CaseInsensitive)) {
                    int v = c + 5;
                    while (v < e && src[v].isSpace()) {
                        ++v;
                    }
                    blockName = src.mid(v, e - v);
                } else if (comment.startsWith(QLatin1String("default:"), Qt::CaseInsensitive)) {
                    int v = c + 8;
                    while (v < e && src[v].isSpace()) {
                        ++v;
                    }
                    int vEnd = src.indexOf(QLatin1String("//"), v);
                    if (vEnd < 0 || vEnd > e) {
                        vEnd = e;
                    }
                    while (vEnd > v && src[vEnd - 1].isSpace()) {
                        --vEnd;
                    }
                    if (vEnd > v) {
                        blockDefStart = v;
                        blockDefEnd = vEnd;
                    }
                }
            }
        } else {
            if (inUniformBlock) {
                flushUniformBlock();
                inUniformBlock = false;
            }
            const QStringView stmt = QStringView(src).mid(s, e - s);
            if (stmt.startsWith(QLatin1String("uniform")) && e - s > 7 && src[s + 7].isSpace()) {
                const int semi = src.indexOf(QLatin1Char(';'), s);
                const int cmt = src.indexOf(QLatin1String("//"), s);
                if (semi > s && cmt > semi && cmt < e) {
                    const QString decl = src.mid(s + 7, semi - (s + 7)).trimmed();
                    const int eqInDecl = decl.indexOf(QLatin1Char('='));
                    const QString cleanDecl =
                        (eqInDecl >= 0) ? decl.left(eqInDecl).trimmed() : decl;
                    int tokEnd = static_cast<int>(cleanDecl.size());
                    while (tokEnd > 0 && cleanDecl[tokEnd - 1].isSpace()) {
                        --tokEnd;
                    }
                    int tokStart = tokEnd;
                    while (tokStart > 0 && !cleanDecl[tokStart - 1].isSpace()) {
                        --tokStart;
                    }
                    QString uname;
                    if (tokStart > 0 && tokEnd > tokStart) {
                        uname = cleanDecl.mid(tokStart, tokEnd - tokStart);
                        const int br = uname.indexOf(QLatin1Char('['));
                        if (br >= 0) {
                            uname = uname.left(br);
                        }
                    }
                    const int bracePos = src.indexOf(QLatin1Char('{'), cmt);
                    int defStart = -1;
                    int defEnd = -1;
                    if (!uname.isEmpty() && bracePos >= 0 && bracePos < e &&
                        findJsonKeySpan(src, bracePos, e, QStringLiteral("default"), defStart,
                                        defEnd)) {
                        m_sourceSpans.push_back(
                            ShaderSourceSpan{uname, QStringLiteral("default"), defStart, defEnd});
                    }
                }
            }
        }

        if (nl < 0) {
            break;
        }
        lineStart = nl + 1;
    }

    flushUniformBlock();
    m_sourceSpansDirty = false;
}

bool GlslSceneScriptEditorWidget::patchUniformOrComboDefaultInSource(const QString &symbolName,
                                                                     const QString &formattedVal)
{
    if (!m_syncDefaultsToCode || symbolName.isEmpty()) {
        return false;
    }
    ensureUiInitialized();
    if (!m_codeEdit) {
        return false;
    }

    const QString fullText = m_codeEdit->toPlainText();
    if (m_sourceSpansDirty) {
        rebuildSourceSpans(fullText);
    }

    auto it = std::ranges::find_if(
        m_sourceSpans, [&](const ShaderSourceSpan &sp) { return sp.symbolName == symbolName; });
    if (it == m_sourceSpans.end()) {
        return false;
    }

    QTextDocument *doc = m_codeEdit->document();
    const int docMax = std::max(0, doc->characterCount() - 1);
    const int spanStart = it->startOffset;
    const int spanEnd = it->endOffset;
    if (spanStart < 0 || spanEnd < spanStart || spanEnd > docMax) {
        return false;
    }

    const QString existing = fullText.mid(spanStart, spanEnd - spanStart);
    if (existing == formattedVal) {
        return true;
    }

    const QTextCursor origCursor = m_codeEdit->textCursor();
    const int origAnchor = origCursor.anchor();
    const int origPos = origCursor.position();
    const int vScroll =
        m_codeEdit->verticalScrollBar() ? m_codeEdit->verticalScrollBar()->value() : 0;
    const int hScroll =
        m_codeEdit->horizontalScrollBar() ? m_codeEdit->horizontalScrollBar()->value() : 0;

    const int delta = static_cast<int>(formattedVal.size()) - (spanEnd - spanStart);
    auto adjustOffset = [&](int pos) -> int {
        if (pos <= spanStart) {
            return pos;
        }
        if (pos >= spanEnd) {
            return pos + delta;
        }
        return std::clamp(pos, spanStart, spanStart + static_cast<int>(formattedVal.size()));
    };

    {
        QScopedValueRollback<bool> guard(m_syncingFromInspector, true);
        QTextCursor editCursor(doc);
        editCursor.setPosition(spanStart);
        editCursor.setPosition(spanEnd, QTextCursor::KeepAnchor);
        editCursor.beginEditBlock();
        editCursor.insertText(formattedVal);
        editCursor.endEditBlock();

        QTextCursor restored = m_codeEdit->textCursor();
        restored.setPosition(adjustOffset(origAnchor));
        restored.setPosition(adjustOffset(origPos), QTextCursor::KeepAnchor);
        m_codeEdit->setTextCursor(restored);

        if (m_codeEdit->verticalScrollBar()) {
            m_codeEdit->verticalScrollBar()->setValue(vScroll);
        }
        if (m_codeEdit->horizontalScrollBar()) {
            m_codeEdit->horizontalScrollBar()->setValue(hScroll);
        }
    }

    it->endOffset = spanStart + static_cast<int>(formattedVal.size());
    for (auto &other : m_sourceSpans) {
        if (&other != &(*it) && other.startOffset >= spanEnd) {
            other.startOffset += delta;
            other.endOffset += delta;
        }
    }

    m_editGeneration.fetch_add(1, std::memory_order_acq_rel);
    if (isVisible() && m_debounceTimer) {
        m_debounceTimer->start(kDebounceValidationMs);
    }

    return true;
}

} // namespace waywal::studio
