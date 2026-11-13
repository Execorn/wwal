// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/wizard/creator_packaging_wizard.hpp"

#include "waywal/log.h"
#include "waywal/path.h"
#include "waywal/shader_graph.h"
#include "waywal/shader_parser.h"
#include "waywal/we_compat_scanner.h"
#include "waywal/we_pkg.h"
#include "waywal/we_project.h"
#include "waywal/wwpkg.h"

#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStyle>
#include <QTextEdit>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>

namespace wwal::studio {

namespace {

void wipeByteArray(QByteArray &buf)
{
    if (!buf.isEmpty()) {
        explicit_bzero(buf.data(), static_cast<size_t>(buf.size()));
        buf.clear();
    }
}

bool isAsciiHex(const QByteArray &buf)
{
    if (buf.isEmpty()) {
        return false;
    }
    for (char ch : buf) {
        const bool ok =
            (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool writeAllFd(int fd, const char *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        const ssize_t n = ::write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

bool isScenePackageType(wwpkg_type_t type)
{
    return type == WWPKG_TYPE_SCENE_2D || type == WWPKG_TYPE_SCENE_PUPPET ||
           type == WWPKG_TYPE_SCENE_PARTICLES;
}

bool isTranscodableTexturePath(const QString &path)
{
    return path.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive) ||
           path.endsWith(QStringLiteral(".jpg"), Qt::CaseInsensitive) ||
           path.endsWith(QStringLiteral(".jpeg"), Qt::CaseInsensitive) ||
           path.endsWith(QStringLiteral(".bmp"), Qt::CaseInsensitive);
}

bool isPathConfinedToRoot(const std::filesystem::path &canonicalRoot, const QString &candidatePath)
{
    if (canonicalRoot.empty() || candidatePath.isEmpty()) {
        return false;
    }
    std::error_code ec;
    const std::filesystem::path candFs(candidatePath.toStdString());
    const std::filesystem::path resolved = std::filesystem::weakly_canonical(candFs, ec);
    if (ec || resolved.empty()) {
        return false;
    }
    auto [rootEnd, candIt] =
        std::mismatch(canonicalRoot.begin(), canonicalRoot.end(), resolved.begin(), resolved.end());
    (void)candIt;
    return rootEnd == canonicalRoot.end();
}

bool isPreviewAssetCandidate(const QString &relPath, const QString &entrypointPreview)
{
    if (!entrypointPreview.isEmpty() &&
        relPath.compare(entrypointPreview, Qt::CaseInsensitive) == 0) {
        return true;
    }
    return relPath.compare(QStringLiteral("preview.png"), Qt::CaseInsensitive) == 0 ||
           relPath.compare(QStringLiteral("preview.jpg"), Qt::CaseInsensitive) == 0 ||
           relPath.compare(QStringLiteral("preview.jpeg"), Qt::CaseInsensitive) == 0 ||
           relPath.compare(QStringLiteral("preview.bmp"), Qt::CaseInsensitive) == 0;
}

bool compileWwgraphFileToGlsl(const QString &wwgraphPath, QByteArray &outGlsl, QString &outTitle,
                              QString &outAuthor, bool &outForceCompute, QString &outError)
{
    outGlsl.clear();
    outError.clear();
    outForceCompute = false;

    QFile f(wwgraphPath);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) {
        outError = QStringLiteral("Shader graph file is missing or unreadable.");
        return false;
    }
    const QByteArray jsonBytes = f.readAll();
    if (jsonBytes.isEmpty() || jsonBytes.size() > 8 * 1024 * 1024) {
        outError = QStringLiteral("Shader graph file must be non-empty and at most 8 MiB.");
        return false;
    }

    waywal_shader_graph_t *graph = waywal_shader_graph_create();
    if (!graph) {
        outError = QStringLiteral("Out of memory allocating shader graph.");
        return false;
    }

    char errBuf[256] = {0};
    if (!waywal_shader_graph_load_json(jsonBytes.constData(), static_cast<size_t>(jsonBytes.size()),
                                       graph, errBuf)) {
        waywal_shader_graph_destroy(graph);
        outError = errBuf[0] != '\0' ? QString::fromUtf8(errBuf)
                                     : QStringLiteral("Invalid .wwgraph JSON document.");
        return false;
    }

    if (graph->graph_name[0] != '\0') {
        outTitle = QString::fromUtf8(graph->graph_name);
    }
    if (graph->author[0] != '\0') {
        outAuthor = QString::fromUtf8(graph->author);
    }
    outForceCompute = (graph->flags & WAYWAL_GRAPH_FLAG_FORCE_COMPUTE) != 0;

    waywal_graph_schedule_t schedule{};
    if (!waywal_shader_graph_analyze(graph, &schedule) || schedule.has_cycle_error) {
        outError = schedule.error_msg[0] != '\0'
                       ? QString::fromUtf8(schedule.error_msg)
                       : QStringLiteral("Shader graph contains an unlatched cycle or invalid pin.");
        waywal_shader_graph_destroy(graph);
        return false;
    }

    size_t glslLen = 0;
    char *glsl = waywal_shader_graph_compile_glsl(graph, &schedule, &glslLen);
    waywal_shader_graph_destroy(graph);
    if (!glsl || glslLen == 0) {
        std::free(glsl);
        outError = QStringLiteral("Failed to generate GLSL ES 3.10 from shader graph.");
        return false;
    }

    outGlsl = QByteArray(glsl, static_cast<qsizetype>(glslLen));
    std::free(glsl);
    return true;
}

bool isDisguisedExecutable(const QString &filePath)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray hdr = f.read(512);
    if (hdr.size() >= 4 && static_cast<uint8_t>(hdr[0]) == 0x7Fu && hdr[1] == 'E' &&
        hdr[2] == 'L' && hdr[3] == 'F') {
        return true;
    }
    if (hdr.size() >= 64 && hdr[0] == 'M' && hdr[1] == 'Z') {
        const uint32_t peOff = static_cast<uint32_t>(static_cast<uint8_t>(hdr[0x3c])) |
                               (static_cast<uint32_t>(static_cast<uint8_t>(hdr[0x3d])) << 8) |
                               (static_cast<uint32_t>(static_cast<uint8_t>(hdr[0x3e])) << 16) |
                               (static_cast<uint32_t>(static_cast<uint8_t>(hdr[0x3f])) << 24);
        if (peOff <= static_cast<uint32_t>(hdr.size()) - 4u) {
            if (hdr[static_cast<qsizetype>(peOff)] == 'P' &&
                hdr[static_cast<qsizetype>(peOff + 1u)] == 'E' &&
                hdr[static_cast<qsizetype>(peOff + 2u)] == '\0' &&
                hdr[static_cast<qsizetype>(peOff + 3u)] == '\0') {
                return true;
            }
        } else if (peOff <= 64u * 1024u * 1024u && f.seek(static_cast<qint64>(peOff))) {
            const QByteArray sig = f.read(4);
            if (sig.size() == 4 && sig[0] == 'P' && sig[1] == 'E' && sig[2] == '\0' &&
                sig[3] == '\0') {
                return true;
            }
        }
    }
    return false;
}

bool hasValidVideoContainerHeader(const QString &filePath)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        return false;
    }
    uint8_t b[256]{};
    const qint64 hdrSize = f.read(reinterpret_cast<char *>(b), sizeof(b));
    if (hdrSize < 12) {
        return false;
    }

    /* Matroska / WebM EBML header */
    if (b[0] == 0x1Au && b[1] == 0x45u && b[2] == 0xDFu && b[3] == 0xA3u) {
        return true;
    }
    /* Ogg bitstream */
    if (memcmp(b, "OggS", 4) == 0) {
        return true;
    }
    /* ISO BMFF / MP4 / MOV top-level box */
    const uint32_t boxSz = (static_cast<uint32_t>(b[0]) << 24) |
                           (static_cast<uint32_t>(b[1]) << 16) |
                           (static_cast<uint32_t>(b[2]) << 8) | static_cast<uint32_t>(b[3]);
    if (boxSz == 0u || boxSz == 1u || boxSz >= 8u) {
        if (memcmp(b + 4, "ftyp", 4) == 0 || memcmp(b + 4, "moov", 4) == 0 ||
            memcmp(b + 4, "mdat", 4) == 0 || memcmp(b + 4, "free", 4) == 0 ||
            memcmp(b + 4, "skip", 4) == 0 || memcmp(b + 4, "wide", 4) == 0) {
            return true;
        }
    }
    /* MPEG-TS sync byte stride */
    if (b[0] == 0x47u && hdrSize >= 189 && b[188] == 0x47u) {
        return true;
    }
    return false;
}

struct ShaderAuditResult {
    we_compat_tier_t tier{WE_TIER_1_NATIVE};
    uint32_t score{100};
    bool usesAudioUniforms{false};
    QList<AuditIssueEntry> issues;
};

ShaderAuditResult auditShaderSource(const QString &absPath, const QString &relPath,
                                    bool isComputeShader)
{
    ShaderAuditResult res;
    QFile f(absPath);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) {
        res.tier = WE_TIER_4_BLOCKED;
        res.score = 0;
        res.issues.append({QStringLiteral("SHADER_ENTRYPOINT_MISSING"), WE_ISSUE_SEV_FATAL,
                           QStringLiteral("Entrypoint shader file is missing or unreadable."),
                           relPath, 0,
                           QStringLiteral("Ensure entrypoint shader exists in stage.")});
        return res;
    }

    const qint64 fileSz = f.size();
    if (fileSz <= 0 || fileSz > 4 * 1024 * 1024) {
        res.tier = WE_TIER_4_BLOCKED;
        res.score = 0;
        res.issues.append({QStringLiteral("SHADER_SIZE_INVALID"), WE_ISSUE_SEV_FATAL,
                           QStringLiteral("Shader file must be non-empty and at most 4 MiB."),
                           relPath, 0,
                           QStringLiteral("Provide a valid GLSL ES 3.10 source file.")});
        return res;
    }

    const QByteArray raw = f.readAll();
    if (raw.contains('\0')) {
        res.tier = WE_TIER_4_BLOCKED;
        res.score = 0;
        res.issues.append({QStringLiteral("SHADER_BINARY_DATA"), WE_ISSUE_SEV_FATAL,
                           QStringLiteral("Shader file contains embedded NUL bytes."), relPath, 0,
                           QStringLiteral("Supply UTF-8 GLSL source code, not binary data.")});
        return res;
    }

    /* Strip comments while preserving line numbers and verifying delimiter balance */
    QByteArray stripped;
    stripped.reserve(raw.size());
    struct DelimFrame {
        char expected;
        uint32_t line;
    };
    QList<DelimFrame> stack;
    uint32_t line = 1;
    const qsizetype n = raw.size();

    for (qsizetype i = 0; i < n; ++i) {
        const char c = raw[i];
        const char next = (i + 1 < n) ? raw[i + 1] : '\0';

        if (c == '\n') {
            stripped.append('\n');
            ++line;
            continue;
        }
        if (c == '/' && next == '/') {
            i += 2;
            while (i < n && raw[i] != '\n') {
                ++i;
            }
            if (i < n && raw[i] == '\n') {
                stripped.append('\n');
                ++line;
            }
            continue;
        }
        if (c == '/' && next == '*') {
            const uint32_t startLine = line;
            i += 2;
            bool closed = false;
            while (i < n) {
                if (raw[i] == '\n') {
                    stripped.append('\n');
                    ++line;
                } else if (raw[i] == '*' && i + 1 < n && raw[i + 1] == '/') {
                    ++i;
                    closed = true;
                    break;
                }
                ++i;
            }
            if (!closed) {
                res.tier = WE_TIER_4_BLOCKED;
                res.score = 0;
                res.issues.append({QStringLiteral("SHADER_UNTERMINATED_COMMENT"),
                                   WE_ISSUE_SEV_FATAL,
                                   QStringLiteral("Unterminated /* block comment in shader."),
                                   relPath, startLine, QStringLiteral("Close the block comment.")});
                return res;
            }
            stripped.append(' ');
            continue;
        }
        if (c == '"') {
            stripped.append(' ');
            ++i;
            while (i < n && raw[i] != '\n') {
                if (raw[i] == '\\' && i + 1 < n && raw[i + 1] != '\n') {
                    i += 2;
                    continue;
                }
                if (raw[i] == '"') {
                    break;
                }
                ++i;
            }
            if (i < n && raw[i] == '\n') {
                stripped.append('\n');
                ++line;
            }
            continue;
        }

        stripped.append(c);
        if (c == '{') {
            stack.append({'}', line});
        } else if (c == '(') {
            stack.append({')', line});
        } else if (c == '[') {
            stack.append({']', line});
        } else if (c == '}' || c == ')' || c == ']') {
            if (stack.isEmpty() || stack.back().expected != c) {
                res.tier = WE_TIER_4_BLOCKED;
                res.score = 0;
                res.issues.append({QStringLiteral("SHADER_SYNTAX_DELIMITER"), WE_ISSUE_SEV_FATAL,
                                   QStringLiteral("Unmatched '%1' delimiter at line %2.")
                                       .arg(QLatin1Char(c))
                                       .arg(line),
                                   relPath, line,
                                   QStringLiteral("Fix mismatched braces or parentheses.")});
                return res;
            }
            stack.removeLast();
        }
    }

    if (!stack.isEmpty()) {
        const DelimFrame unclosed = stack.back();
        res.tier = WE_TIER_4_BLOCKED;
        res.score = 0;
        res.issues.append(
            {QStringLiteral("SHADER_SYNTAX_DELIMITER"), WE_ISSUE_SEV_FATAL,
             QStringLiteral("Unclosed delimiter before end of shader (opened at line %1).")
                 .arg(unclosed.line),
             relPath, unclosed.line, QStringLiteral("Balance all '{', '(', and '[' blocks.")});
        return res;
    }

    const QString code = QString::fromUtf8(stripped);
    static const QRegularExpression mainRe(QStringLiteral(R"(\bvoid\s+(main|mainImage)\s*\()"));
    if (!mainRe.match(code).hasMatch()) {
        res.tier = WE_TIER_4_BLOCKED;
        res.score = 0;
        res.issues.append(
            {QStringLiteral("SHADER_MAIN_MISSING"), WE_ISSUE_SEV_FATAL,
             QStringLiteral("Shader must define 'void main()' or 'void mainImage()'."), relPath, 1,
             QStringLiteral("Add a GLSL ES 3.10 entry point function.")});
        return res;
    }

    if (isComputeShader) {
        static const QRegularExpression localSizeRe(
            QStringLiteral(R"(\blocal_size_x\s*=\s*[1-9]\d*)"));
        if (!localSizeRe.match(code).hasMatch()) {
            res.tier = WE_TIER_4_BLOCKED;
            res.score = 0;
            res.issues.append(
                {QStringLiteral("COMPUTE_LAYOUT_MISSING"), WE_ISSUE_SEV_FATAL,
                 QStringLiteral("Compute shader is missing layout(local_size_x = ...) qualifier."),
                 relPath, 1,
                 QStringLiteral("Declare layout(local_size_x = 16, local_size_y = 16) in;")});
            return res;
        }
    }

    static const QRegularExpression versionRe(
        QStringLiteral(R"(^\s*#\s*version\s+(\d+)(?:\s+(\w+))?)"),
        QRegularExpression::MultilineOption);
    const QRegularExpressionMatch vm = versionRe.match(code);
    bool hasEs3 = false;
    if (vm.hasMatch()) {
        const int ver = vm.captured(1).toInt();
        const QString profile = vm.captured(2).toLower();
        if (profile == QStringLiteral("es") && ver >= 300) {
            hasEs3 = true;
        } else if (profile != QStringLiteral("es")) {
            res.tier = WE_TIER_2_TRANSPILED;
            res.score = qMin(res.score, 90u);
            res.issues.append(
                {QStringLiteral("SHADER_DESKTOP_GLSL"), WE_ISSUE_SEV_WARNING,
                 QStringLiteral(
                     "Desktop '#version %1' header will be adapted to '#version 310 es'.")
                     .arg(ver),
                 relPath, 1, QStringLiteral("Target '#version 310 es' directly.")});
        }
    }

    if (!isComputeShader && hasEs3) {
        static const QRegularExpression precRe(
            QStringLiteral(R"(\bprecision\s+(lowp|mediump|highp)\s+float\s*;)"));
        if (!precRe.match(code).hasMatch()) {
            res.score = (res.score >= 5u) ? (res.score - 5u) : 0u;
            res.issues.append({QStringLiteral("SHADER_FLOAT_PRECISION"), WE_ISSUE_SEV_WARNING,
                               QStringLiteral("GLSL ES 3.x fragment shader lacks default float "
                                              "precision qualifier."),
                               relPath, 1, QStringLiteral("Add 'precision highp float;'")});
        }
    }

    static const QRegularExpression legacyBuiltinRe(
        QStringLiteral(R"(\b(gl_FragColor|texture2D)\b)"));
    static const QRegularExpression mainImageOnlyRe(QStringLiteral(R"(\bvoid\s+mainImage\s*\()"));
    if (legacyBuiltinRe.match(code).hasMatch() && !mainImageOnlyRe.match(code).hasMatch()) {
        res.tier = WE_TIER_2_TRANSPILED;
        res.score = qMin(res.score, 85u);
        res.issues.append({QStringLiteral("SHADER_LEGACY_BUILTINS"), WE_ISSUE_SEV_WARNING,
                           QStringLiteral("Legacy gl_FragColor/texture2D calls require ES 3.10 "
                                          "compatibility defines."),
                           relPath, 1,
                           QStringLiteral("Use 'out vec4 fragColor;' and 'texture()'.")});
    }

    static const QRegularExpression audioRe(
        QStringLiteral(R"(\b(iAudio|u_audio|audio_spectrum|u_spectrum|u_AudioBands)\b)"));
    res.usesAudioUniforms = audioRe.match(code).hasMatch();

    return res;
}

/* Helper to format byte counts into human-readable strings */
QString formatBytes(qint64 bytes)
{
    if (bytes < 1024) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    if (bytes < 1024 * 1024) {
        return QStringLiteral("%1 KiB").arg(QString::number(bytes / 1024.0, 'f', 1));
    }
    if (bytes < 1024 * 1024 * 1024) {
        return QStringLiteral("%1 MiB").arg(QString::number(bytes / (1024.0 * 1024.0), 'f', 2));
    }
    return QStringLiteral("%1 GiB").arg(
        QString::number(bytes / (1024.0 * 1024.0 * 1024.0), 'f', 2));
}

/* Helper to convert title string into canonical RFC-compliant slug */
QString slugify(const QString &title)
{
    QString s = title.toLower();
    QString out;
    bool lastWasHyphen = true; // prevent leading hyphen

    for (const QChar &ch : s) {
        if ((ch >= u'a' && ch <= u'z') || (ch >= u'0' && ch <= u'9')) {
            out.append(ch);
            lastWasHyphen = false;
        } else if (!lastWasHyphen) {
            out.append(u'-');
            lastWasHyphen = true;
        }
    }

    while (out.endsWith(u'-')) {
        out.chop(1);
    }
    if (out.isEmpty()) {
        out = QStringLiteral("untitled-package");
    }
    return out;
}

void addPubkeyToTrustedKeys(const QString &pubkeyHex)
{
    const QString home = QDir::homePath();
    const QString trustedPath = home + QStringLiteral("/.config/waywal/trusted_keys.json");
    QFileInfo fi(trustedPath);
    fi.dir().mkpath(QStringLiteral("."));

    QJsonObject rootObj;
    QJsonArray keysArray;
    QFile readFile(trustedPath);
    if (readFile.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(readFile.readAll());
        readFile.close();
        if (doc.isObject()) {
            rootObj = doc.object();
            if (rootObj.contains(QStringLiteral("trusted_keys"))) {
                keysArray = rootObj.value(QStringLiteral("trusted_keys")).toArray();
            }
        } else if (doc.isArray()) {
            keysArray = doc.array();
        }
    }

    bool alreadyPresent = false;
    for (const auto &val : keysArray) {
        if (val.isString() && val.toString().compare(pubkeyHex, Qt::CaseInsensitive) == 0) {
            alreadyPresent = true;
            break;
        }
        if (val.isObject()) {
            const QJsonObject obj = val.toObject();
            const QString k = obj.contains(QStringLiteral("pubkey"))
                                  ? obj.value(QStringLiteral("pubkey")).toString()
                                  : obj.value(QStringLiteral("key")).toString();
            if (k.compare(pubkeyHex, Qt::CaseInsensitive) == 0 &&
                !obj.value(QStringLiteral("revoked")).toBool(false)) {
                alreadyPresent = true;
                break;
            }
        }
    }

    if (!alreadyPresent) {
        keysArray.append(pubkeyHex.toLower());
        rootObj.insert(QStringLiteral("trusted_keys"), keysArray);
        QJsonDocument writeDoc(rootObj);
        QFile writeFile(trustedPath);
        if (writeFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            writeFile.write(writeDoc.toJson(QJsonDocument::Indented));
            writeFile.close();
        }
    }
}

bool inspectPkgSummary(const QString &pkgPath, int &outEntryCount, qint64 &outTotalBytes,
                       QString &outTitle, QString &outType, QString &outEntrypoint,
                       QString &outPreview)
{
    outEntryCount = 0;
    outTotalBytes = 0;
    we_pkg_t *pkg = we_pkg_open_file(pkgPath.toUtf8().constData(), nullptr);
    if (!pkg) {
        return false;
    }

    outEntryCount = static_cast<int>(pkg->entry_count);
    for (uint32_t i = 0; i < pkg->entry_count; ++i) {
        outTotalBytes += static_cast<qint64>(pkg->entries[i].length);
    }

    size_t projSz = 0;
    if (const uint8_t *projData = we_pkg_find(pkg, "project.json", &projSz);
        projData && projSz > 0) {
        const QJsonDocument doc = QJsonDocument::fromJson(
            QByteArray(reinterpret_cast<const char *>(projData), static_cast<qsizetype>(projSz)));
        if (doc.isObject()) {
            const QJsonObject obj = doc.object();
            if (outTitle.isEmpty()) {
                outTitle = obj.value(QStringLiteral("title")).toString();
            }
            if (outType.isEmpty()) {
                outType = obj.value(QStringLiteral("type")).toString();
            }
            if (outEntrypoint.isEmpty()) {
                outEntrypoint = obj.value(QStringLiteral("file")).toString();
            }
            if (outPreview.isEmpty()) {
                outPreview = obj.value(QStringLiteral("preview")).toString();
            }
        }
    }

    if (outEntrypoint.isEmpty() && we_pkg_find(pkg, "scene.json", nullptr) != nullptr) {
        outEntrypoint = QStringLiteral("scene.json");
    }

    we_pkg_close(pkg);
    return true;
}

bool unpackPkgIntoStaging(const QString &pkgPath, const QString &stagingRoot)
{
    we_pkg_t *pkg = we_pkg_open_file(pkgPath.toUtf8().constData(), nullptr);
    if (!pkg) {
        return false;
    }

    QDir stagingDir(stagingRoot);
    stagingDir.mkpath(QStringLiteral("."));
    std::error_code rootEc;
    const std::filesystem::path canonicalRoot =
        std::filesystem::weakly_canonical(std::filesystem::path(stagingRoot.toStdString()), rootEc);

    bool ok = (pkg->entry_count > 0);
    for (uint32_t i = 0; i < pkg->entry_count; ++i) {
        const we_pkg_entry_t &e = pkg->entries[i];
        char normPath[WE_PKG_MAX_PATH_LEN + 1];
        if (!we_pkg_normalize_path(e.path, std::strlen(e.path), normPath, sizeof(normPath))) {
            ok = false;
            continue;
        }
        const QString rel = QString::fromUtf8(normPath);
        if (rel.isEmpty()) {
            continue;
        }
        const QString dstPath = stagingDir.filePath(rel);
        QFileInfo(dstPath).dir().mkpath(QStringLiteral("."));
        if (!rootEc && !isPathConfinedToRoot(canonicalRoot, dstPath)) {
            ok = false;
            continue;
        }
        QFile out(dstPath);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            ok = false;
            continue;
        }
        if (e.length > 0 && e.data) {
            const qint64 written =
                out.write(reinterpret_cast<const char *>(e.data), static_cast<qint64>(e.length));
            if (written != static_cast<qint64>(e.length)) {
                ok = false;
            }
        }
        if (!out.flush() || out.error() != QFileDevice::NoError) {
            ok = false;
        }
        out.close();
    }

    we_pkg_close(pkg);
    return ok;
}

bool writeMinimalSceneWrapper(const QDir &staging, const QString &title, const QString &texRelPath)
{
    if (!staging.exists(QStringLiteral("scene.json"))) {
        if (texRelPath.isEmpty() || !staging.exists(texRelPath)) {
            return false;
        }
        QJsonObject texObj;
        texObj.insert(QStringLiteral("image"), texRelPath);
        texObj.insert(QStringLiteral("name"), QStringLiteral("Layer 0"));
        texObj.insert(QStringLiteral("id"), 1);
        texObj.insert(QStringLiteral("origin"), QStringLiteral("960 540 0"));
        texObj.insert(QStringLiteral("scale"), QStringLiteral("1 1 1"));
        texObj.insert(QStringLiteral("size"), QStringLiteral("1920 1080"));
        QJsonObject sceneObj;
        sceneObj.insert(QStringLiteral("general"),
                        QJsonObject{{QStringLiteral("orthogonalprojection"),
                                     QJsonObject{{QStringLiteral("width"), 1920},
                                                 {QStringLiteral("height"), 1080}}}});
        sceneObj.insert(QStringLiteral("objects"), QJsonArray{texObj});
        QFile sf(staging.filePath(QStringLiteral("scene.json")));
        if (!sf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return false;
        }
        const QByteArray sceneBytes = QJsonDocument(sceneObj).toJson(QJsonDocument::Indented);
        if (sf.write(sceneBytes) != sceneBytes.size() || !sf.flush()) {
            return false;
        }
        sf.close();
    }
    if (!staging.exists(QStringLiteral("project.json"))) {
        QJsonObject projObj;
        projObj.insert(QStringLiteral("title"), title);
        projObj.insert(QStringLiteral("type"), QStringLiteral("scene"));
        projObj.insert(QStringLiteral("file"), QStringLiteral("scene.json"));
        QFile pf(staging.filePath(QStringLiteral("project.json")));
        if (!pf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return false;
        }
        const QByteArray projBytes = QJsonDocument(projObj).toJson(QJsonDocument::Indented);
        if (pf.write(projBytes) != projBytes.size() || !pf.flush()) {
            return false;
        }
        pf.close();
    }
    return true;
}

} // namespace

/* ========================================================================= */
/* Step 1: SourceIntakePage Implementation                                   */
/* ========================================================================= */

SourceIntakePage::SourceIntakePage(CreatorPackagingContext *ctx, QWidget *parent)
    : QWizardPage(parent), m_ctx(ctx)
{
    setTitle(QStringLiteral("Step 1: Source Files"));
    setSubTitle(QStringLiteral("Select a wallpaper project folder or shader file to package."));
    setupUi();
}

void SourceIntakePage::setupUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(12);

    // Source path group
    auto *pathGroup = new QGroupBox(QStringLiteral("Project Source"), this);
    auto *pathLayout = new QVBoxLayout(pathGroup);
    pathLayout->setSpacing(10);

    auto *rowLayout = new QHBoxLayout();
    rowLayout->setSpacing(8);
    m_sourcePathEdit = new QLineEdit(pathGroup);
    m_sourcePathEdit->setPlaceholderText(
        QStringLiteral("Select project folder, shader (.frag/.comp), or scene.json..."));
    rowLayout->addWidget(m_sourcePathEdit, 1);

    m_browseDirBtn = new QPushButton(QStringLiteral("Browse Folder..."), pathGroup);
    rowLayout->addWidget(m_browseDirBtn);

    m_browseFileBtn = new QPushButton(QStringLiteral("Browse File..."), pathGroup);
    rowLayout->addWidget(m_browseFileBtn);

    pathLayout->addLayout(rowLayout);

    // Project Type dropdown
    auto *typeLayout = new QHBoxLayout();
    typeLayout->setSpacing(8);
    auto *typeLabel = new QLabel(QStringLiteral("Package Type:"), pathGroup);
    typeLabel->setFixedWidth(110);
    typeLayout->addWidget(typeLabel);

    m_typeCombo = new QComboBox(pathGroup);
    m_typeCombo->addItem(QStringLiteral("Fragment Shader"),
                         QVariant(static_cast<int>(WWPKG_TYPE_SHADER_FRAGMENT)));
    m_typeCombo->addItem(QStringLiteral("Compute Shader"),
                         QVariant(static_cast<int>(WWPKG_TYPE_SHADER_COMPUTE)));
    m_typeCombo->addItem(QStringLiteral("2D Scene"),
                         QVariant(static_cast<int>(WWPKG_TYPE_SCENE_2D)));
    m_typeCombo->addItem(QStringLiteral("Puppet Scene"),
                         QVariant(static_cast<int>(WWPKG_TYPE_SCENE_PUPPET)));
    m_typeCombo->addItem(QStringLiteral("Particle Scene"),
                         QVariant(static_cast<int>(WWPKG_TYPE_SCENE_PARTICLES)));
    m_typeCombo->addItem(QStringLiteral("Video Wallpaper"),
                         QVariant(static_cast<int>(WWPKG_TYPE_VIDEO_STREAM)));
    typeLayout->addWidget(m_typeCombo, 1);

    pathLayout->addLayout(typeLayout);
    layout->addWidget(pathGroup);

    // Analysis results card
    auto *analysisGroup = new QGroupBox(QStringLiteral("Project Summary"), this);
    auto *analysisLayout = new QGridLayout(analysisGroup);
    analysisLayout->setHorizontalSpacing(16);
    analysisLayout->setVerticalSpacing(8);

    analysisLayout->addWidget(new QLabel(QStringLiteral("Title:"), analysisGroup), 0, 0);
    m_detectedTitleLabel = new QLabel(QStringLiteral("None"), analysisGroup);
    m_detectedTitleLabel->setStyleSheet(QStringLiteral("font-weight: bold; color: #38bdf8;"));
    analysisLayout->addWidget(m_detectedTitleLabel, 0, 1);

    analysisLayout->addWidget(new QLabel(QStringLiteral("Author:"), analysisGroup), 1, 0);
    m_detectedAuthorLabel = new QLabel(QStringLiteral("None"), analysisGroup);
    analysisLayout->addWidget(m_detectedAuthorLabel, 1, 1);

    analysisLayout->addWidget(new QLabel(QStringLiteral("Entrypoint:"), analysisGroup), 2, 0);
    m_entrypointLabel = new QLabel(QStringLiteral("shaders/main.frag"), analysisGroup);
    m_entrypointLabel->setStyleSheet(QStringLiteral("font-family: monospace; color: #34d399;"));
    analysisLayout->addWidget(m_entrypointLabel, 2, 1);

    analysisLayout->addWidget(new QLabel(QStringLiteral("Files:"), analysisGroup), 3, 0);
    m_fileCountLabel = new QLabel(QStringLiteral("0 files (0 B)"), analysisGroup);
    analysisLayout->addWidget(m_fileCountLabel, 3, 1);

    layout->addWidget(analysisGroup);

    // Status message
    m_statusLabel =
        new QLabel(QStringLiteral("Select a project directory or shader file to continue."), this);
    m_statusLabel->setStyleSheet(QStringLiteral("color: #94a3b8;"));
    layout->addWidget(m_statusLabel);

    layout->addStretch();

    // Wiring
    connect(m_browseDirBtn, &QPushButton::clicked, this, [this]() {
        QString dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Select Project Directory"), m_sourcePathEdit->text());
        if (!dir.isEmpty()) {
            m_sourcePathEdit->setText(dir);
        }
    });

    connect(m_browseFileBtn, &QPushButton::clicked, this, [this]() {
        QString file = QFileDialog::getOpenFileName(
            this, QStringLiteral("Select Shader, Graph, or Scene File"), m_sourcePathEdit->text(),
            QStringLiteral("All Supported (*.frag *.vert *.comp *.glsl *.wwgraph *.json *.pkg "
                           "*.tex *.mp4 *.webm *.mkv);;"
                           "Shader Graphs (*.wwgraph);;"
                           "GLSL Shaders (*.frag *.vert *.comp *.glsl);;"
                           "Scene / Project JSON (*.json);;"
                           "WE Scene Packages (*.pkg);;"
                           "WE Textures (*.tex);;"
                           "Videos (*.mp4 *.webm *.mkv);;"
                           "All Files (*)"));
        if (!file.isEmpty()) {
            m_sourcePathEdit->setText(file);
        }
    });

    connect(m_sourcePathEdit, &QLineEdit::textChanged, this,
            [this](const QString &text) { analyzeSourcePath(text.trimmed()); });

    connect(m_typeCombo, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (m_ctx) {
            m_ctx->projectType = static_cast<wwpkg_type_t>(m_typeCombo->itemData(idx).toInt());
            switch (m_ctx->projectType) {
            case WWPKG_TYPE_SHADER_FRAGMENT:
                m_ctx->projectTypeStr = QStringLiteral("shader_fragment");
                break;
            case WWPKG_TYPE_SHADER_COMPUTE:
                m_ctx->projectTypeStr = QStringLiteral("shader_compute");
                break;
            case WWPKG_TYPE_SCENE_2D:
                m_ctx->projectTypeStr = QStringLiteral("scene_2d");
                break;
            case WWPKG_TYPE_SCENE_PUPPET:
                m_ctx->projectTypeStr = QStringLiteral("scene_puppet");
                break;
            case WWPKG_TYPE_SCENE_PARTICLES:
                m_ctx->projectTypeStr = QStringLiteral("scene_particles");
                break;
            case WWPKG_TYPE_VIDEO_STREAM:
                m_ctx->projectTypeStr = QStringLiteral("video_stream");
                break;
            default:
                m_ctx->projectTypeStr = QStringLiteral("shader_fragment");
                break;
            }

            const QFileInfo srcFi(m_ctx->sourcePath);
            if (srcFi.exists() && srcFi.isFile()) {
                const QString ext = srcFi.suffix().toLower();
                if (m_ctx->projectType == WWPKG_TYPE_SHADER_FRAGMENT) {
                    m_ctx->entrypointPipeline = QStringLiteral("shaders/main.frag");
                } else if (m_ctx->projectType == WWPKG_TYPE_SHADER_COMPUTE) {
                    m_ctx->entrypointPipeline = QStringLiteral("shaders/main.comp");
                } else if (m_ctx->projectType == WWPKG_TYPE_VIDEO_STREAM) {
                    m_ctx->entrypointPipeline =
                        (ext == QStringLiteral("webm") || ext == QStringLiteral("mkv"))
                            ? (QStringLiteral("assets/video.") + ext)
                            : QStringLiteral("assets/video.mp4");
                } else if (isScenePackageType(m_ctx->projectType)) {
                    m_ctx->entrypointPipeline = QStringLiteral("scene.json");
                }
                if (m_entrypointLabel) {
                    m_entrypointLabel->setText(m_ctx->entrypointPipeline);
                }
            }
        }
    });
}

void SourceIntakePage::setSourcePath(const QString &path)
{
    if (m_sourcePathEdit) {
        m_sourcePathEdit->setText(path);
    }
}

void SourceIntakePage::initializePage()
{
    if (m_ctx && !m_ctx->sourcePath.isEmpty()) {
        m_sourcePathEdit->setText(m_ctx->sourcePath);
    }
}

void SourceIntakePage::analyzeSourcePath(const QString &path)
{
    if (!m_ctx)
        return;

    m_ctx->sourcePath = path;
    m_ctx->entrypointPipeline.clear();
    m_ctx->detectedTitle.clear();
    m_ctx->detectedAuthor.clear();
    m_ctx->detectedDescription.clear();
    m_ctx->detectedLicense.clear();
    QFileInfo info(path);

    if (path.isEmpty() || !info.exists()) {
        m_detectedTitleLabel->setText(QStringLiteral("None"));
        m_detectedAuthorLabel->setText(QStringLiteral("None"));
        m_entrypointLabel->setText(QStringLiteral("None"));
        m_fileCountLabel->setText(QStringLiteral("0 files (0 B)"));
        m_statusLabel->setText(
            path.isEmpty()
                ? QStringLiteral("Select a project directory or shader file to continue.")
                : QStringLiteral("Selected path does not exist."));
        m_statusLabel->setStyleSheet(QStringLiteral("color: #ef4444;"));
        emit completeChanged();
        return;
    }

    qint64 totalBytes = 0;
    int fileCount = 0;

    if (info.isDir()) {
        QDir dir(path);

        // 1. Check for Wallpaper Engine project.json
        if (dir.exists(QStringLiteral("project.json"))) {
            we_project_t proj;
            memset(&proj, 0, sizeof(proj));
            if (we_project_load_dir(path.toUtf8().constData(), &proj)) {
                if (proj.title[0] != '\0') {
                    m_ctx->detectedTitle = QString::fromUtf8(proj.title);
                }
                QString pType = QString::fromUtf8(proj.type).toLower();
                if (pType == QStringLiteral("video")) {
                    m_typeCombo->setCurrentIndex(5); // video_stream
                } else if (pType == QStringLiteral("scene")) {
                    m_typeCombo->setCurrentIndex(2); // scene_2d
                }
                if (proj.file[0] != '\0') {
                    m_ctx->entrypointPipeline = QString::fromUtf8(proj.file);
                } else {
                    m_ctx->entrypointPipeline = QStringLiteral("scene.json");
                }
                if (proj.preview[0] != '\0') {
                    m_ctx->entrypointPreview = QString::fromUtf8(proj.preview);
                }
            }
        }

        // 1b. Check packed scene.pkg if loose project.json didn't set a pipeline
        if (m_ctx->entrypointPipeline.isEmpty() && dir.exists(QStringLiteral("scene.pkg"))) {
            int pkgEntries = 0;
            qint64 pkgBytes = 0;
            QString pkgType;
            if (inspectPkgSummary(dir.filePath(QStringLiteral("scene.pkg")), pkgEntries, pkgBytes,
                                  m_ctx->detectedTitle, pkgType, m_ctx->entrypointPipeline,
                                  m_ctx->entrypointPreview)) {
                m_typeCombo->setCurrentIndex(2); // scene_2d
            }
        }

        // 2. Check for GLSL shaders and visual shader graphs (.wwgraph)
        if (m_ctx->entrypointPipeline.isEmpty()) {
            if (dir.exists(QStringLiteral("shaders/main.frag"))) {
                m_ctx->entrypointPipeline = QStringLiteral("shaders/main.frag");
            } else if (dir.exists(QStringLiteral("main.frag"))) {
                m_ctx->entrypointPipeline = QStringLiteral("main.frag");
            } else if (dir.exists(QStringLiteral("shaders/main.wwgraph"))) {
                m_ctx->entrypointPipeline = QStringLiteral("shaders/main.wwgraph");
            } else if (dir.exists(QStringLiteral("main.wwgraph"))) {
                m_ctx->entrypointPipeline = QStringLiteral("main.wwgraph");
            } else if (dir.exists(QStringLiteral("shaders/main.comp"))) {
                m_ctx->entrypointPipeline = QStringLiteral("shaders/main.comp");
                m_typeCombo->setCurrentIndex(1); // shader_compute
            } else {
                const QStringList graphs =
                    dir.entryList({QStringLiteral("*.wwgraph")}, QDir::Files, QDir::Name);
                if (!graphs.isEmpty()) {
                    m_ctx->entrypointPipeline = graphs.front();
                }
            }
        }

        // Parse shader or graph metadata if present
        QString fragCandidate = dir.filePath(m_ctx->entrypointPipeline);
        if (QFile::exists(fragCandidate) &&
            fragCandidate.endsWith(QStringLiteral(".wwgraph"), Qt::CaseInsensitive)) {
            QByteArray glslBytes;
            QString graphTitle;
            QString graphAuthor;
            QString graphErr;
            bool forceCompute = false;
            (void)compileWwgraphFileToGlsl(fragCandidate, glslBytes, graphTitle, graphAuthor,
                                           forceCompute, graphErr);
            if (m_ctx->detectedTitle.isEmpty() && !graphTitle.isEmpty()) {
                m_ctx->detectedTitle = graphTitle;
            }
            if (!graphAuthor.isEmpty()) {
                m_ctx->detectedAuthor = graphAuthor;
            }
            m_typeCombo->setCurrentIndex(forceCompute ? 1 : 0);
        } else if (QFile::exists(fragCandidate) &&
                   (fragCandidate.endsWith(QStringLiteral(".frag")) ||
                    fragCandidate.endsWith(QStringLiteral(".glsl")))) {
            waywal_shader_meta_t meta;
            waywal_shader_meta_init(&meta);
            if (waywal_shader_parse_file(fragCandidate.toUtf8().constData(), &meta)) {
                if (m_ctx->detectedTitle.isEmpty() && meta.title[0] != '\0') {
                    m_ctx->detectedTitle = QString::fromUtf8(meta.title);
                }
                if (meta.author[0] != '\0') {
                    m_ctx->detectedAuthor = QString::fromUtf8(meta.author);
                }
                if (meta.description[0] != '\0') {
                    m_ctx->detectedDescription = QString::fromUtf8(meta.description);
                }
                if (meta.license[0] != '\0') {
                    m_ctx->detectedLicense = QString::fromUtf8(meta.license);
                }
                if (meta.has_multi_pass || meta.passes[1].target_fps > 0) {
                    m_typeCombo->setCurrentIndex(1); // shader_compute
                }
            }
        }

        // Count confined files and total size, skipping external symlinks
        std::error_code rootEc;
        const std::filesystem::path canonicalRoot =
            std::filesystem::weakly_canonical(std::filesystem::path(path.toStdString()), rootEc);
        QDirIterator it(path, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString fPath = it.next();
            if (!rootEc && !isPathConfinedToRoot(canonicalRoot, fPath)) {
                continue;
            }
            fileCount++;
            totalBytes += it.fileInfo().size();
        }
    } else {
        // Single file intake
        fileCount = 1;
        totalBytes = info.size();
        const QString ext = info.suffix().toLower();

        if (ext == QStringLiteral("wwgraph")) {
            QByteArray glslBytes;
            QString graphTitle;
            QString graphAuthor;
            QString graphErr;
            bool forceCompute = false;
            const bool ok = compileWwgraphFileToGlsl(path, glslBytes, graphTitle, graphAuthor,
                                                     forceCompute, graphErr);
            m_typeCombo->setCurrentIndex(forceCompute ? 1 : 0);
            m_ctx->entrypointPipeline =
                ok ? QStringLiteral("shaders/main.frag") : QStringLiteral("shaders/graph.wwgraph");
            if (!graphTitle.isEmpty()) {
                m_ctx->detectedTitle = graphTitle;
            }
            if (!graphAuthor.isEmpty()) {
                m_ctx->detectedAuthor = graphAuthor;
            }
        } else if (ext == QStringLiteral("frag") || ext == QStringLiteral("glsl")) {
            m_typeCombo->setCurrentIndex(0); // shader_fragment
            m_ctx->entrypointPipeline = QStringLiteral("shaders/main.frag");
            waywal_shader_meta_t meta;
            waywal_shader_meta_init(&meta);
            if (waywal_shader_parse_file(path.toUtf8().constData(), &meta)) {
                if (meta.title[0] != '\0')
                    m_ctx->detectedTitle = QString::fromUtf8(meta.title);
                if (meta.author[0] != '\0')
                    m_ctx->detectedAuthor = QString::fromUtf8(meta.author);
                if (meta.description[0] != '\0')
                    m_ctx->detectedDescription = QString::fromUtf8(meta.description);
                if (meta.license[0] != '\0')
                    m_ctx->detectedLicense = QString::fromUtf8(meta.license);
            }
        } else if (ext == QStringLiteral("comp")) {
            m_typeCombo->setCurrentIndex(1); // shader_compute
            m_ctx->entrypointPipeline = QStringLiteral("shaders/main.comp");
        } else if (ext == QStringLiteral("mp4") || ext == QStringLiteral("webm") ||
                   ext == QStringLiteral("mkv")) {
            m_typeCombo->setCurrentIndex(5); // video_stream
            m_ctx->entrypointPipeline =
                (ext == QStringLiteral("webm") || ext == QStringLiteral("mkv"))
                    ? (QStringLiteral("assets/video.") + ext)
                    : QStringLiteral("assets/video.mp4");
        } else if (ext == QStringLiteral("json")) {
            m_typeCombo->setCurrentIndex(2); // scene_2d
            m_ctx->entrypointPipeline = QStringLiteral("scene.json");
        } else if (ext == QStringLiteral("pkg")) {
            m_typeCombo->setCurrentIndex(2); // scene_2d
            m_ctx->entrypointPipeline = QStringLiteral("scene.json");
            QString pkgType;
            int pkgCount = 0;
            qint64 pkgBytes = 0;
            if (inspectPkgSummary(path, pkgCount, pkgBytes, m_ctx->detectedTitle, pkgType,
                                  m_ctx->entrypointPipeline, m_ctx->entrypointPreview)) {
                fileCount = std::max(1, pkgCount);
                totalBytes = std::max(totalBytes, pkgBytes);
            }
        } else if (ext == QStringLiteral("tex")) {
            m_typeCombo->setCurrentIndex(2); // scene_2d
            m_ctx->entrypointPipeline = QStringLiteral("scene.json");
        }
    }

    if (m_ctx->detectedTitle.isEmpty()) {
        m_ctx->detectedTitle = info.isDir() ? info.fileName() : info.completeBaseName();
    }
    if (m_ctx->entrypointPipeline.isEmpty()) {
        m_ctx->entrypointPipeline = QStringLiteral("shaders/main.frag");
    }

    m_detectedTitleLabel->setText(m_ctx->detectedTitle);
    m_detectedAuthorLabel->setText(m_ctx->detectedAuthor.isEmpty() ? QStringLiteral("Unknown")
                                                                   : m_ctx->detectedAuthor);
    m_entrypointLabel->setText(m_ctx->entrypointPipeline);
    m_fileCountLabel->setText(
        QStringLiteral("%1 files (%2)").arg(fileCount).arg(formatBytes(totalBytes)));

    m_statusLabel->setText(QStringLiteral("Source ready: %1 file(s) found.").arg(fileCount));
    m_statusLabel->setStyleSheet(QStringLiteral("color: #10b981; font-weight: bold;"));

    emit completeChanged();
}

bool SourceIntakePage::isComplete() const
{
    return m_ctx && !m_ctx->sourcePath.isEmpty() && QFileInfo::exists(m_ctx->sourcePath);
}

bool SourceIntakePage::validatePage()
{
    if (!isComplete()) {
        return false;
    }
    m_ctx->projectType = static_cast<wwpkg_type_t>(m_typeCombo->currentData().toInt());
    return true;
}

/* ========================================================================= */
/* Step 2: AssetBundlingPage Implementation                                  */
/* ========================================================================= */

AssetBundlingPage::AssetBundlingPage(CreatorPackagingContext *ctx, QWidget *parent)
    : QWizardPage(parent), m_ctx(ctx)
{
    setTitle(QStringLiteral("Step 2: Assets"));
    setSubTitle(QStringLiteral("Review included files and compression settings."));
    setupUi();
}

void AssetBundlingPage::setupUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(10);

    m_summaryLabel = new QLabel(QStringLiteral("Scanning assets..."), this);
    m_summaryLabel->setStyleSheet(QStringLiteral("color: #94a3b8; font-size: 11px;"));
    layout->addWidget(m_summaryLabel);

    m_assetTree = new QTreeWidget(this);
    m_assetTree->setColumnCount(4);
    m_assetTree->setHeaderLabels({QStringLiteral("File"), QStringLiteral("Type"),
                                  QStringLiteral("Size"), QStringLiteral("Action")});
    m_assetTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_assetTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_assetTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_assetTree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_assetTree->setAlternatingRowColors(true);
    layout->addWidget(m_assetTree, 1);

    auto *optGroup = new QGroupBox(QStringLiteral("Asset Options"), this);
    auto *optLayout = new QVBoxLayout(optGroup);
    optLayout->setSpacing(8);

    m_pruneCheck = new QCheckBox(
        QStringLiteral("Exclude temporary and unused files (.git, .DS_Store, *.bak)"), optGroup);
    m_pruneCheck->setChecked(true);
    optLayout->addWidget(m_pruneCheck);

    m_transcodeCheck =
        new QCheckBox(QStringLiteral("Convert PNG, JPEG, and BMP textures to WebP"), optGroup);
    m_transcodeCheck->setChecked(true);
    optLayout->addWidget(m_transcodeCheck);

    layout->addWidget(optGroup);

    auto *btnLayout = new QHBoxLayout();
    m_rescanBtn = new QPushButton(QStringLiteral("Rescan Assets"), this);
    btnLayout->addWidget(m_rescanBtn);
    btnLayout->addStretch();
    layout->addLayout(btnLayout);

    connect(m_rescanBtn, &QPushButton::clicked, this, &AssetBundlingPage::scanAssets);
}

void AssetBundlingPage::initializePage() { scanAssets(); }

void AssetBundlingPage::scanAssets()
{
    if (!m_ctx || m_ctx->sourcePath.isEmpty())
        return;

    m_assetTree->clear();
    m_ctx->scannedAssets.clear();

    QFileInfo srcInfo(m_ctx->sourcePath);
    qint64 totalSize = 0;
    int deadCount = 0;

    if (srcInfo.isDir()) {
        std::error_code rootEc;
        const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(
            std::filesystem::path(m_ctx->sourcePath.toStdString()), rootEc);
        QDir baseDir(m_ctx->sourcePath);
        QDirIterator it(m_ctx->sourcePath, QDir::Files | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);

        while (it.hasNext()) {
            const QString absPath = it.next();
            if (!rootEc && !isPathConfinedToRoot(canonicalRoot, absPath)) {
                deadCount++;
                continue;
            }
            QString relPath = baseDir.relativeFilePath(absPath);
            qint64 sz = it.fileInfo().size();
            totalSize += sz;

            ScannedAssetEntry entry;
            entry.relativePath = relPath;
            entry.sizeBytes = sz;

            const QString lower = relPath.toLower();
            if (lower.startsWith(QStringLiteral(".git")) ||
                lower.startsWith(QStringLiteral(".ds_store")) ||
                lower.endsWith(QStringLiteral(".bak")) || lower.endsWith(QStringLiteral(".tmp")) ||
                lower.endsWith(QStringLiteral("~"))) {
                entry.category = QStringLiteral("temporary");
                entry.isReferenced = false;
                deadCount++;
            } else if (lower.endsWith(QStringLiteral(".frag")) ||
                       lower.endsWith(QStringLiteral(".vert")) ||
                       lower.endsWith(QStringLiteral(".comp")) ||
                       lower.endsWith(QStringLiteral(".glsl")) ||
                       lower.endsWith(QStringLiteral(".wwgraph"))) {
                entry.category = QStringLiteral("shader");
            } else if (lower.endsWith(QStringLiteral(".png")) ||
                       lower.endsWith(QStringLiteral(".jpg")) ||
                       lower.endsWith(QStringLiteral(".jpeg")) ||
                       lower.endsWith(QStringLiteral(".webp")) ||
                       lower.endsWith(QStringLiteral(".bmp")) ||
                       lower.endsWith(QStringLiteral(".tga")) ||
                       lower.endsWith(QStringLiteral(".tex"))) {
                entry.category = QStringLiteral("texture");
            } else if (lower.endsWith(QStringLiteral(".pkg"))) {
                entry.category = QStringLiteral("scene-package");
            } else if (lower.endsWith(QStringLiteral(".mdl")) ||
                       lower.endsWith(QStringLiteral(".glb")) ||
                       lower.endsWith(QStringLiteral(".gltf")) ||
                       lower.endsWith(QStringLiteral(".obj"))) {
                entry.category = QStringLiteral("model");
            } else if (lower.endsWith(QStringLiteral(".opus")) ||
                       lower.endsWith(QStringLiteral(".ogg")) ||
                       lower.endsWith(QStringLiteral(".flac")) ||
                       lower.endsWith(QStringLiteral(".mp3"))) {
                entry.category = QStringLiteral("audio");
            } else if (lower.endsWith(QStringLiteral(".mp4")) ||
                       lower.endsWith(QStringLiteral(".webm")) ||
                       lower.endsWith(QStringLiteral(".mkv"))) {
                entry.category = QStringLiteral("video");
            } else if (lower.endsWith(QStringLiteral(".js")) ||
                       lower.endsWith(QStringLiteral(".json"))) {
                entry.category = QStringLiteral("script/data");
            } else {
                entry.category = QStringLiteral("other");
            }

            m_ctx->scannedAssets.append(entry);

            auto *item = new QTreeWidgetItem(m_assetTree);
            item->setText(0, entry.relativePath);
            item->setText(1, entry.category);
            item->setText(2, formatBytes(entry.sizeBytes));

            if (!entry.isReferenced) {
                item->setText(3, QStringLiteral("Exclude"));
                item->setForeground(3, QBrush(QColor(QStringLiteral("#ef4444"))));
            } else if (entry.category == QStringLiteral("texture") &&
                       isTranscodableTexturePath(lower) &&
                       isPreviewAssetCandidate(entry.relativePath, m_ctx->entrypointPreview)) {
                item->setText(3, QStringLiteral("Convert Preview to WebP"));
                item->setForeground(3, QBrush(QColor(QStringLiteral("#38bdf8"))));
            } else {
                item->setText(3, QStringLiteral("Include"));
                item->setForeground(3, QBrush(QColor(QStringLiteral("#34d399"))));
            }
        }
    } else {
        ScannedAssetEntry entry;
        entry.relativePath = srcInfo.fileName();
        entry.sizeBytes = srcInfo.size();
        if (m_ctx->projectType == WWPKG_TYPE_VIDEO_STREAM) {
            entry.category = QStringLiteral("video");
        } else if (srcInfo.suffix().compare(QStringLiteral("pkg"), Qt::CaseInsensitive) == 0) {
            entry.category = QStringLiteral("scene-package");
        } else if (srcInfo.suffix().compare(QStringLiteral("tex"), Qt::CaseInsensitive) == 0) {
            entry.category = QStringLiteral("texture");
        } else if (isScenePackageType(m_ctx->projectType)) {
            entry.category = QStringLiteral("script/data");
        } else {
            entry.category = QStringLiteral("shader");
        }
        entry.isReferenced = true;
        totalSize += entry.sizeBytes;

        m_ctx->scannedAssets.append(entry);

        auto *item = new QTreeWidgetItem(m_assetTree);
        item->setText(0, entry.relativePath);
        item->setText(1, entry.category);
        item->setText(2, formatBytes(entry.sizeBytes));
        item->setText(3, QStringLiteral("Stage as %1").arg(m_ctx->entrypointPipeline));
        item->setForeground(3, QBrush(QColor(QStringLiteral("#34d399"))));
    }

    m_summaryLabel->setText(QStringLiteral("Found %1 file(s) (%2). %3 marked for exclusion.")
                                .arg(m_ctx->scannedAssets.size())
                                .arg(formatBytes(totalSize))
                                .arg(deadCount));
}

bool AssetBundlingPage::validatePage()
{
    if (!m_ctx)
        return false;

    m_ctx->pruneDeadFiles = m_pruneCheck->isChecked();
    m_ctx->transcodeTextures = m_transcodeCheck->isChecked();

    if (m_ctx->stagingDir.isEmpty()) {
        m_ctx->stagingDir = QDir::tempPath() + QStringLiteral("/wwpkg_stage_") +
                            QUuid::createUuid().toString(QUuid::WithoutBraces);
    } else if (m_ctx->stagingDir.startsWith(QDir::tempPath())) {
        QDir(m_ctx->stagingDir).removeRecursively();
    }
    QDir staging(m_ctx->stagingDir);
    staging.mkpath(QStringLiteral("."));

    QFileInfo srcInfo(m_ctx->sourcePath);
    if (srcInfo.isDir()) {
        std::error_code rootEc;
        const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(
            std::filesystem::path(m_ctx->sourcePath.toStdString()), rootEc);
        QDir srcDir(m_ctx->sourcePath);
        QString updatedPreviewRel;

        for (const auto &asset : m_ctx->scannedAssets) {
            if (m_ctx->pruneDeadFiles && !asset.isReferenced) {
                continue;
            }
            QString srcFile = srcDir.filePath(asset.relativePath);
            if (!rootEc && !isPathConfinedToRoot(canonicalRoot, srcFile)) {
                continue;
            }
            if (asset.category == QStringLiteral("scene-package") &&
                !srcDir.exists(QStringLiteral("scene.json"))) {
                if (!unpackPkgIntoStaging(srcFile, m_ctx->stagingDir)) {
                    return false;
                }
                continue;
            }
            QString dstFile = staging.filePath(asset.relativePath);

            QFileInfo(dstFile).dir().mkpath(QStringLiteral("."));
            QFile::remove(dstFile);
            QFile::copy(srcFile, dstFile);

            // Only transcode standalone preview thumbnails whose manifest/project.json reference
            // is updated in lockstep; keep referenced scene/shader/graph texture extensions intact.
            if (m_ctx->transcodeTextures && asset.category == QStringLiteral("texture") &&
                isTranscodableTexturePath(asset.relativePath) &&
                isPreviewAssetCandidate(asset.relativePath, m_ctx->entrypointPreview)) {
                QImage img(dstFile);
                if (!img.isNull()) {
                    QString webpDst = dstFile;
                    const int dot = webpDst.lastIndexOf(u'.');
                    if (dot > 0) {
                        webpDst.truncate(dot);
                    }
                    webpDst.append(QStringLiteral(".webp"));
                    if (webpDst != dstFile && img.save(webpDst, "WEBP", 85)) {
                        QFile::remove(dstFile);
                        QString relWebp = asset.relativePath;
                        const int relDot = relWebp.lastIndexOf(u'.');
                        if (relDot > 0) {
                            relWebp.truncate(relDot);
                        }
                        relWebp.append(QStringLiteral(".webp"));
                        m_ctx->entrypointPreview = relWebp;
                        updatedPreviewRel = relWebp;
                    }
                }
            }
        }

        if (!updatedPreviewRel.isEmpty()) {
            const QString projJsonPath = staging.filePath(QStringLiteral("project.json"));
            QFile pf(projJsonPath);
            if (pf.exists() && pf.open(QIODevice::ReadOnly)) {
                QJsonDocument doc = QJsonDocument::fromJson(pf.readAll());
                pf.close();
                if (doc.isObject()) {
                    QJsonObject obj = doc.object();
                    obj.insert(QStringLiteral("preview"), updatedPreviewRel);
                    if (pf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                        pf.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
                        pf.close();
                    }
                }
            }
        }

        if (m_ctx->entrypointPipeline.endsWith(QStringLiteral(".wwgraph"), Qt::CaseInsensitive)) {
            const QString stagedGraph = staging.filePath(m_ctx->entrypointPipeline);
            QByteArray glslBytes;
            QString graphTitle;
            QString graphAuthor;
            QString graphErr;
            bool forceCompute = false;
            if (compileWwgraphFileToGlsl(stagedGraph, glslBytes, graphTitle, graphAuthor,
                                         forceCompute, graphErr)) {
                const QString compiledRel = QStringLiteral("shaders/main.frag");
                const QString compiledDst = staging.filePath(compiledRel);
                QFileInfo(compiledDst).dir().mkpath(QStringLiteral("."));
                QFile outFrag(compiledDst);
                if (outFrag.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    outFrag.write(glslBytes);
                    outFrag.close();
                    m_ctx->entrypointPipeline = compiledRel;
                }
            }
        }
    } else {
        const QString ext = srcInfo.suffix().toLower();
        if (ext == QStringLiteral("pkg")) {
            const bool unpacked = unpackPkgIntoStaging(m_ctx->sourcePath, m_ctx->stagingDir);
            if (!unpacked) {
                return false;
            }
            if (m_ctx->entrypointPipeline.isEmpty()) {
                m_ctx->entrypointPipeline = QStringLiteral("scene.json");
            }
            QString fallbackTex;
            if (!staging.exists(QStringLiteral("scene.json"))) {
                if (staging.exists(QStringLiteral("materials/default.tex"))) {
                    fallbackTex = QStringLiteral("materials/default.tex");
                } else {
                    QDirIterator texIt(m_ctx->stagingDir,
                                       {QStringLiteral("*.tex"), QStringLiteral("*.png"),
                                        QStringLiteral("*.jpg"), QStringLiteral("*.webp")},
                                       QDir::Files, QDirIterator::Subdirectories);
                    if (texIt.hasNext()) {
                        fallbackTex = staging.relativeFilePath(texIt.next());
                    }
                }
                if (fallbackTex.isEmpty()) {
                    return false;
                }
            }
            return writeMinimalSceneWrapper(
                staging,
                m_ctx->detectedTitle.isEmpty() ? srcInfo.completeBaseName() : m_ctx->detectedTitle,
                fallbackTex);
        }
        if (ext == QStringLiteral("tex")) {
            const QString texRel = QStringLiteral("materials/") + srcInfo.fileName();
            const QString texDst = staging.filePath(texRel);
            QFileInfo(texDst).dir().mkpath(QStringLiteral("."));
            QFile::remove(texDst);
            if (!QFile::copy(m_ctx->sourcePath, texDst)) {
                return false;
            }
            m_ctx->entrypointPipeline = QStringLiteral("scene.json");
            return writeMinimalSceneWrapper(
                staging,
                m_ctx->detectedTitle.isEmpty() ? srcInfo.completeBaseName() : m_ctx->detectedTitle,
                texRel);
        }
        if (ext == QStringLiteral("wwgraph")) {
            const QString graphDst = staging.filePath(QStringLiteral("shaders/graph.wwgraph"));
            QFileInfo(graphDst).dir().mkpath(QStringLiteral("."));
            QFile::remove(graphDst);
            QFile::copy(m_ctx->sourcePath, graphDst);

            QByteArray glslBytes;
            QString graphTitle;
            QString graphAuthor;
            QString graphErr;
            bool forceCompute = false;
            if (compileWwgraphFileToGlsl(graphDst, glslBytes, graphTitle, graphAuthor, forceCompute,
                                         graphErr)) {
                const QString compiledRel = QStringLiteral("shaders/main.frag");
                const QString compiledDst = staging.filePath(compiledRel);
                QFile outFrag(compiledDst);
                if (outFrag.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    outFrag.write(glslBytes);
                    outFrag.close();
                    m_ctx->entrypointPipeline = compiledRel;
                } else {
                    m_ctx->entrypointPipeline = QStringLiteral("shaders/graph.wwgraph");
                }
            } else {
                m_ctx->entrypointPipeline = QStringLiteral("shaders/graph.wwgraph");
            }
            return true;
        }

        QString relDst = m_ctx->entrypointPipeline;
        if (m_ctx->projectType == WWPKG_TYPE_SHADER_FRAGMENT) {
            relDst = QStringLiteral("shaders/main.frag");
        } else if (m_ctx->projectType == WWPKG_TYPE_SHADER_COMPUTE) {
            relDst = QStringLiteral("shaders/main.comp");
        } else if (m_ctx->projectType == WWPKG_TYPE_VIDEO_STREAM) {
            if (!relDst.startsWith(QStringLiteral("assets/"))) {
                relDst = (ext == QStringLiteral("webm") || ext == QStringLiteral("mkv"))
                             ? (QStringLiteral("assets/video.") + ext)
                             : QStringLiteral("assets/video.mp4");
            }
        } else if (relDst.isEmpty()) {
            relDst = srcInfo.fileName();
        }
        m_ctx->entrypointPipeline = relDst;

        const QString dst = staging.filePath(relDst);
        QFileInfo(dst).dir().mkpath(QStringLiteral("."));
        QFile::remove(dst);
        QFile::copy(m_ctx->sourcePath, dst);
    }

    return true;
}

/* ========================================================================= */
/* Step 3: MetadataPreviewPage Implementation                                */
/* ========================================================================= */

MetadataPreviewPage::MetadataPreviewPage(CreatorPackagingContext *ctx, QWidget *parent)
    : QWizardPage(parent), m_ctx(ctx)
{
    setTitle(QStringLiteral("Step 3: Details & Preview"));
    setSubTitle(QStringLiteral("Set the package name, version, license, and preview image."));
    setupUi();
}

void MetadataPreviewPage::setupUi()
{
    auto *mainLayout = new QHBoxLayout(this);
    mainLayout->setSpacing(16);

    // Left column: Metadata fields
    auto *metaGroup = new QGroupBox(QStringLiteral("Package Details"), this);
    auto *form = new QGridLayout(metaGroup);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(8);

    int row = 0;

    // Package ID
    form->addWidget(new QLabel(QStringLiteral("Package ID:"), metaGroup), row, 0);
    m_idEdit = new QLineEdit(metaGroup);
    m_idEdit->setReadOnly(true);
    m_idEdit->setStyleSheet(QStringLiteral("font-family: monospace; color: #94a3b8;"));
    form->addWidget(m_idEdit, row++, 1, 1, 2);

    // Slug
    form->addWidget(new QLabel(QStringLiteral("Slug:"), metaGroup), row, 0);
    m_slugEdit = new QLineEdit(metaGroup);
    m_slugEdit->setPlaceholderText(QStringLiteral("lowercase-hyphenated-name"));
    form->addWidget(m_slugEdit, row, 1);
    m_slugValidationLabel = new QLabel(QStringLiteral("Valid"), metaGroup);
    m_slugValidationLabel->setStyleSheet(QStringLiteral("color: #10b981; font-weight: bold;"));
    form->addWidget(m_slugValidationLabel, row++, 2);

    // Name
    form->addWidget(new QLabel(QStringLiteral("Title:"), metaGroup), row, 0);
    m_nameEdit = new QLineEdit(metaGroup);
    m_nameEdit->setPlaceholderText(QStringLiteral("Wallpaper title"));
    form->addWidget(m_nameEdit, row++, 1, 1, 2);

    // Version
    form->addWidget(new QLabel(QStringLiteral("Version:"), metaGroup), row, 0);
    m_versionEdit = new QLineEdit(metaGroup);
    m_versionEdit->setPlaceholderText(QStringLiteral("1.0.0"));
    form->addWidget(m_versionEdit, row, 1);
    m_versionValidationLabel = new QLabel(QStringLiteral("Valid"), metaGroup);
    m_versionValidationLabel->setStyleSheet(QStringLiteral("color: #10b981; font-weight: bold;"));
    form->addWidget(m_versionValidationLabel, row++, 2);

    // Author Name & Email
    form->addWidget(new QLabel(QStringLiteral("Author:"), metaGroup), row, 0);
    m_authorNameEdit = new QLineEdit(metaGroup);
    form->addWidget(m_authorNameEdit, row++, 1, 1, 2);

    form->addWidget(new QLabel(QStringLiteral("Email:"), metaGroup), row, 0);
    m_authorEmailEdit = new QLineEdit(metaGroup);
    m_authorEmailEdit->setPlaceholderText(QStringLiteral("author@example.com"));
    form->addWidget(m_authorEmailEdit, row++, 1, 1, 2);

    // SPDX License
    form->addWidget(new QLabel(QStringLiteral("License:"), metaGroup), row, 0);
    m_licenseCombo = new QComboBox(metaGroup);
    m_licenseCombo->setEditable(true);
    m_licenseCombo->addItems(
        {QStringLiteral("MIT"), QStringLiteral("Apache-2.0"), QStringLiteral("GPL-3.0-or-later"),
         QStringLiteral("GPL-2.0-or-later"), QStringLiteral("BSD-3-Clause"),
         QStringLiteral("CC-BY-4.0"), QStringLiteral("CC0-1.0"), QStringLiteral("Unlicense")});
    form->addWidget(m_licenseCombo, row++, 1, 1, 2);

    // Tags
    form->addWidget(new QLabel(QStringLiteral("Tags:"), metaGroup), row, 0);
    m_tagsEdit = new QLineEdit(metaGroup);
    m_tagsEdit->setPlaceholderText(QStringLiteral("abstract, audio-reactive, 60fps"));
    form->addWidget(m_tagsEdit, row++, 1, 1, 2);

    // Target FPS & VRAM
    auto *budgetRow = new QHBoxLayout();
    budgetRow->setSpacing(8);
    budgetRow->addWidget(new QLabel(QStringLiteral("Target FPS:"), metaGroup));
    m_fpsCombo = new QComboBox(metaGroup);
    m_fpsCombo->addItems(
        {QStringLiteral("30"), QStringLiteral("60"), QStringLiteral("120"), QStringLiteral("144")});
    m_fpsCombo->setCurrentText(QStringLiteral("60"));
    budgetRow->addWidget(m_fpsCombo);

    budgetRow->addWidget(new QLabel(QStringLiteral("VRAM Limit:"), metaGroup));
    m_vramSpin = new QSpinBox(metaGroup);
    m_vramSpin->setRange(32, 2048);
    m_vramSpin->setValue(256);
    m_vramSpin->setSuffix(QStringLiteral(" MB"));
    budgetRow->addWidget(m_vramSpin);

    form->addLayout(budgetRow, row++, 0, 1, 3);

    // Description
    form->addWidget(new QLabel(QStringLiteral("Description:"), metaGroup), row, 0);
    m_descriptionEdit = new QTextEdit(metaGroup);
    m_descriptionEdit->setPlaceholderText(QStringLiteral("Short description of the wallpaper..."));
    m_descriptionEdit->setMaximumHeight(80);
    form->addWidget(m_descriptionEdit, row++, 1, 1, 2);

    mainLayout->addWidget(metaGroup, 3);

    // Right column: Preview Capture Box
    auto *previewGroup = new QGroupBox(QStringLiteral("Preview Image"), this);
    auto *previewLayout = new QVBoxLayout(previewGroup);
    previewLayout->setSpacing(10);

    m_previewDisplay = new QLabel(previewGroup);
    m_previewDisplay->setFixedSize(256, 144);
    m_previewDisplay->setStyleSheet(QStringLiteral(
        "border: 1px solid #2c3240; background-color: #12141a; border-radius: 4px;"));
    m_previewDisplay->setAlignment(Qt::AlignCenter);
    previewLayout->addWidget(m_previewDisplay, 0, Qt::AlignCenter);

    m_previewInfoLabel = new QLabel(QStringLiteral("512x288 WebP & 1280x720 JPEG"), previewGroup);
    m_previewInfoLabel->setStyleSheet(QStringLiteral("color: #94a3b8; font-size: 11px;"));
    m_previewInfoLabel->setAlignment(Qt::AlignCenter);
    previewLayout->addWidget(m_previewInfoLabel);

    m_captureBtn = new QPushButton(QStringLiteral("Generate Preview"), previewGroup);
    previewLayout->addWidget(m_captureBtn);

    m_browsePreviewBtn = new QPushButton(QStringLiteral("Load Custom Image..."), previewGroup);
    previewLayout->addWidget(m_browsePreviewBtn);

    previewLayout->addStretch();
    mainLayout->addWidget(previewGroup, 2);

    // Validation connections
    connect(m_slugEdit, &QLineEdit::textChanged, this, &MetadataPreviewPage::validateFields);
    connect(m_nameEdit, &QLineEdit::textChanged, this, &MetadataPreviewPage::validateFields);
    connect(m_versionEdit, &QLineEdit::textChanged, this, &MetadataPreviewPage::validateFields);
    connect(m_authorNameEdit, &QLineEdit::textChanged, this, &MetadataPreviewPage::validateFields);

    connect(m_captureBtn, &QPushButton::clicked, this,
            &MetadataPreviewPage::captureOffscreenPreview);
    connect(m_browsePreviewBtn, &QPushButton::clicked, this,
            &MetadataPreviewPage::loadCustomPreview);
}

void MetadataPreviewPage::initializePage()
{
    if (!m_ctx)
        return;

    if (m_ctx->packageId.isEmpty()) {
        m_ctx->packageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    m_idEdit->setText(m_ctx->packageId);

    if (m_nameEdit->text().isEmpty()) {
        m_nameEdit->setText(m_ctx->detectedTitle.isEmpty() ? QStringLiteral("WayWal Wallpaper")
                                                           : m_ctx->detectedTitle);
    }

    if (m_slugEdit->text().isEmpty()) {
        m_slugEdit->setText(slugify(m_nameEdit->text()));
    }

    if (m_versionEdit->text().isEmpty()) {
        m_versionEdit->setText(QStringLiteral("1.0.0"));
    }

    if (m_authorNameEdit->text().isEmpty()) {
        QString author = m_ctx->detectedAuthor;
        if (author.isEmpty()) {
            author = qEnvironmentVariable("USER");
            if (author.isEmpty())
                author = QStringLiteral("Creator");
        }
        m_authorNameEdit->setText(author);
    }

    if (m_authorEmailEdit->text().isEmpty()) {
        QString email = qEnvironmentVariable("EMAIL");
        if (email.isEmpty())
            email = QStringLiteral("creator@waywal.org");
        m_authorEmailEdit->setText(email);
    }

    if (!m_ctx->detectedDescription.isEmpty() && m_descriptionEdit->toPlainText().isEmpty()) {
        m_descriptionEdit->setText(m_ctx->detectedDescription);
    }

    // Check if preview already exists in staging
    bool loadedExisting = false;
    if (!m_ctx->stagingDir.isEmpty()) {
        QDir stg(m_ctx->stagingDir);
        for (const QString &c : {QStringLiteral("preview.webp"), QStringLiteral("preview_card.jpg"),
                                 QStringLiteral("preview.jpg"), QStringLiteral("preview.png")}) {
            if (stg.exists(c)) {
                QImage img(stg.filePath(c));
                if (!img.isNull()) {
                    m_ctx->previewImage = img;
                    loadedExisting = true;
                    break;
                }
            }
        }
    }

    if (!loadedExisting && m_ctx->previewImage.isNull()) {
        captureOffscreenPreview();
    } else {
        updatePreviewDisplay();
    }

    validateFields();
}

void MetadataPreviewPage::validateFields()
{
    static const QRegularExpression slugRegex(QStringLiteral("^[a-z0-9]+(-[a-z0-9]+)*$"));
    static const QRegularExpression semverRegex(
        QStringLiteral(R"(^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$)"));

    const QString slug = m_slugEdit->text().trimmed();
    const bool slugValid = slugRegex.match(slug).hasMatch();
    if (slugValid) {
        m_slugValidationLabel->setText(QStringLiteral("Valid"));
        m_slugValidationLabel->setStyleSheet(QStringLiteral("color: #10b981; font-weight: bold;"));
    } else {
        m_slugValidationLabel->setText(QStringLiteral("Use lowercase and hyphens"));
        m_slugValidationLabel->setStyleSheet(QStringLiteral("color: #ef4444; font-size: 10px;"));
    }

    const QString version = m_versionEdit->text().trimmed();
    const bool versionValid = semverRegex.match(version).hasMatch();
    if (versionValid) {
        m_versionValidationLabel->setText(QStringLiteral("Valid"));
        m_versionValidationLabel->setStyleSheet(
            QStringLiteral("color: #10b981; font-weight: bold;"));
    } else {
        m_versionValidationLabel->setText(QStringLiteral("Use X.Y.Z"));
        m_versionValidationLabel->setStyleSheet(QStringLiteral("color: #ef4444; font-size: 10px;"));
    }

    emit completeChanged();
}

bool MetadataPreviewPage::isComplete() const
{
    static const QRegularExpression slugRegex(QStringLiteral("^[a-z0-9]+(-[a-z0-9]+)*$"));
    static const QRegularExpression semverRegex(
        QStringLiteral(R"(^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$)"));

    return slugRegex.match(m_slugEdit->text().trimmed()).hasMatch() &&
           semverRegex.match(m_versionEdit->text().trimmed()).hasMatch() &&
           !m_nameEdit->text().trimmed().isEmpty() &&
           !m_authorNameEdit->text().trimmed().isEmpty() && m_ctx && !m_ctx->previewImage.isNull();
}

void MetadataPreviewPage::captureOffscreenPreview()
{
    // Generate an offscreen 512x288 preview thumbnail canvas
    QImage canvas(512, 288, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);

    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing);

    // Deep modern dark radial gradient
    QRadialGradient bgGrad(256, 144, 300, 200, 100);
    bgGrad.setColorAt(0.0, QColor(0x1e, 0x1b, 0x4b));
    bgGrad.setColorAt(0.7, QColor(0x0f, 0x17, 0x2a));
    bgGrad.setColorAt(1.0, QColor(0x02, 0x06, 0x17));
    painter.fillRect(canvas.rect(), bgGrad);

    // Subtle geometry pattern
    painter.setPen(QPen(QColor(0x38, 0xbd, 0xf8, 0x18), 1));
    for (int x = 0; x < 512; x += 32) {
        painter.drawLine(x, 0, x, 288);
    }
    for (int y = 0; y < 288; y += 32) {
        painter.drawLine(0, y, 512, y);
    }

    // Centered accent circle
    QLinearGradient circleGrad(256, 70, 256, 170);
    circleGrad.setColorAt(0.0, QColor(0x63, 0x66, 0xf1, 0x90));
    circleGrad.setColorAt(1.0, QColor(0x06, 0xb6, 0xd4, 0x40));
    painter.setBrush(circleGrad);
    painter.setPen(QPen(QColor(0x38, 0xbd, 0xf8, 0x80), 2));
    painter.drawEllipse(QPoint(256, 110), 48, 48);

    // Monogram label
    QFont monoFont(QStringLiteral("Sans Serif"), 18, QFont::Bold);
    painter.setFont(monoFont);
    painter.setPen(QPen(Qt::white, 2));
    painter.drawText(QRect(208, 62, 96, 96), Qt::AlignCenter, QStringLiteral("WW"));

    // Title typography
    QFont titleFont(QStringLiteral("Sans Serif"), 16, QFont::Bold);
    painter.setFont(titleFont);
    painter.setPen(Qt::white);
    QString title = m_nameEdit ? m_nameEdit->text() : QStringLiteral("WayWal Wallpaper");
    if (title.isEmpty())
        title = QStringLiteral("WayWal Wallpaper");
    painter.drawText(QRect(20, 175, 472, 30), Qt::AlignCenter, title);

    // Author & subtitle
    QFont subFont(QStringLiteral("Sans Serif"), 10);
    painter.setFont(subFont);
    painter.setPen(QColor(0x94, 0xa3, 0xb8));
    QString author = m_authorNameEdit ? m_authorNameEdit->text() : QStringLiteral("Creator");
    painter.drawText(QRect(20, 205, 472, 20), Qt::AlignCenter,
                     QStringLiteral("By %1 | WayWal Open Hub").arg(author));

    // Badge pill
    painter.setBrush(QColor(0x0f, 0x76, 0x6e, 0xa0));
    painter.setPen(QPen(QColor(0x14, 0xb8, 0xa6), 1));
    painter.drawRoundedRect(QRect(186, 240, 140, 22), 11, 11);
    QFont badgeFont(QStringLiteral("Monospace"), 8, QFont::Bold);
    painter.setFont(badgeFont);
    painter.setPen(QColor(0x5e, 0xeb, 0xd3));
    painter.drawText(QRect(186, 240, 140, 22), Qt::AlignCenter,
                     QStringLiteral("GLES 3.1 | 60 FPS"));

    painter.end();

    if (m_ctx) {
        m_ctx->previewImage = canvas;
    }
    updatePreviewDisplay();
    validateFields();
}

void MetadataPreviewPage::loadCustomPreview()
{
    QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Select Preview Image"), QString(),
        QStringLiteral("Images (*.png *.jpg *.jpeg *.webp *.bmp);;All Files (*)"));
    if (!path.isEmpty()) {
        QImage img(path);
        if (!img.isNull()) {
            if (m_ctx) {
                m_ctx->previewImage = img;
            }
            updatePreviewDisplay();
            validateFields();
        }
    }
}

void MetadataPreviewPage::updatePreviewDisplay()
{
    if (!m_ctx || m_ctx->previewImage.isNull()) {
        m_previewDisplay->setText(QStringLiteral("No Preview"));
        return;
    }

    QPixmap pix = QPixmap::fromImage(m_ctx->previewImage.scaled(
        256, 144, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
    m_previewDisplay->setPixmap(pix);
}

bool MetadataPreviewPage::validatePage()
{
    if (!isComplete())
        return false;

    m_ctx->slug = m_slugEdit->text().trimmed();
    m_ctx->name = m_nameEdit->text().trimmed();
    m_ctx->version = m_versionEdit->text().trimmed();
    m_ctx->authorName = m_authorNameEdit->text().trimmed();
    m_ctx->authorEmail = m_authorEmailEdit->text().trimmed();
    m_ctx->license = m_licenseCombo->currentText().trimmed();
    m_ctx->description = m_descriptionEdit->toPlainText().trimmed();
    m_ctx->targetFps = m_fpsCombo->currentText().toUInt();
    m_ctx->maxVramMb = static_cast<uint32_t>(m_vramSpin->value());

    m_ctx->tags.clear();
    const QStringList rawTags = m_tagsEdit->text().split(u',', Qt::SkipEmptyParts);
    for (const QString &t : rawTags) {
        QString clean = t.trimmed();
        if (!clean.isEmpty() && m_ctx->tags.size() < 16) {
            m_ctx->tags.append(clean);
        }
    }

    // Save preview images into staging area
    if (!m_ctx->stagingDir.isEmpty() && !m_ctx->previewImage.isNull()) {
        QDir stg(m_ctx->stagingDir);
        m_ctx->previewWebpPath = stg.filePath(QStringLiteral("preview.webp"));
        m_ctx->previewCardJpgPath = stg.filePath(QStringLiteral("preview_card.jpg"));

        QImage p512 =
            m_ctx->previewImage.scaled(512, 288, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        if (p512.save(m_ctx->previewWebpPath, "WEBP", 90)) {
            for (const QString &stale :
                 {QStringLiteral("preview.png"), QStringLiteral("preview.jpg"),
                  QStringLiteral("preview.jpeg"), QStringLiteral("preview.bmp")}) {
                QFile::remove(stg.filePath(stale));
            }
        }

        QImage p1280 =
            m_ctx->previewImage.scaled(1280, 720, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        p1280.save(m_ctx->previewCardJpgPath, "JPEG", 90);
    }

    return true;
}

/* ========================================================================= */
/* Step 4: CapabilityAuditPage Implementation                                */
/* ========================================================================= */

CapabilityAuditPage::CapabilityAuditPage(CreatorPackagingContext *ctx, QWidget *parent)
    : QWizardPage(parent), m_ctx(ctx)
{
    setTitle(QStringLiteral("Step 4: Compatibility Check"));
    setSubTitle(QStringLiteral("Review shader compatibility and required permissions."));
    setupUi();
}

void CapabilityAuditPage::setupUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(12);

    // Top: Classification & Score Bar
    auto *badgeBox = new QGroupBox(QStringLiteral("Compatibility"), this);
    auto *badgeLayout = new QGridLayout(badgeBox);
    badgeLayout->setHorizontalSpacing(16);
    badgeLayout->setVerticalSpacing(8);

    m_tierBadge = new QLabel(QStringLiteral("Native"), badgeBox);
    m_tierBadge->setStyleSheet(
        QStringLiteral("background-color: #10b981; color: white; font-weight: bold; "
                       "padding: 6px 12px; border-radius: 4px; font-size: 13px;"));
    m_tierBadge->setAlignment(Qt::AlignCenter);
    m_tierBadge->setFixedWidth(180);
    badgeLayout->addWidget(m_tierBadge, 0, 0);

    m_scoreLabel = new QLabel(QStringLiteral("Compatibility Score: 100/100"), badgeBox);
    m_scoreLabel->setStyleSheet(QStringLiteral("font-weight: bold; font-size: 12px;"));
    badgeLayout->addWidget(m_scoreLabel, 0, 1);

    m_scoreBar = new QProgressBar(badgeBox);
    m_scoreBar->setRange(0, 100);
    m_scoreBar->setValue(100);
    m_scoreBar->setFixedHeight(16);
    badgeLayout->addWidget(m_scoreBar, 1, 0, 1, 2);

    m_metricsLabel = new QLabel(
        QStringLiteral("Estimated VRAM: 64.0 MB | Target FPS: 60 | Passes: 6"), badgeBox);
    m_metricsLabel->setStyleSheet(QStringLiteral("color: #94a3b8; font-size: 11px;"));
    badgeLayout->addWidget(m_metricsLabel, 2, 0, 1, 2);

    layout->addWidget(badgeBox);

    // Capabilities Checklist
    auto *capGroup = new QGroupBox(QStringLiteral("Permissions"), this);
    auto *capLayout = new QGridLayout(capGroup);
    capLayout->setHorizontalSpacing(20);
    capLayout->setVerticalSpacing(8);

    m_capComputeCheck = new QCheckBox(QStringLiteral("Compute Shaders (GLES 3.1)"), capGroup);
    capLayout->addWidget(m_capComputeCheck, 0, 0);

    m_capAudioSpectrumCheck = new QCheckBox(QStringLiteral("Audio Spectrum Uniforms"), capGroup);
    capLayout->addWidget(m_capAudioSpectrumCheck, 0, 1);

    m_capAudioPlaybackCheck = new QCheckBox(QStringLiteral("Audio Playback"), capGroup);
    capLayout->addWidget(m_capAudioPlaybackCheck, 1, 0);

    m_capScriptingCheck = new QCheckBox(QStringLiteral("SceneScript (QuickJS)"), capGroup);
    capLayout->addWidget(m_capScriptingCheck, 1, 1);

    m_capForeignToplevelCheck = new QCheckBox(QStringLiteral("Window State Tracking"), capGroup);
    capLayout->addWidget(m_capForeignToplevelCheck, 2, 0);

    // Locked air-gap flags
    m_capNetworkCheck = new QCheckBox(QStringLiteral("Network Access (Disabled)"), capGroup);
    m_capNetworkCheck->setChecked(false);
    m_capNetworkCheck->setEnabled(false);
    m_capNetworkCheck->setToolTip(
        QStringLiteral("Wallpaper packages run offline without network access."));
    m_capNetworkCheck->setStyleSheet(QStringLiteral("color: #94a3b8;"));
    capLayout->addWidget(m_capNetworkCheck, 3, 0);

    m_capFilesystemCheck =
        new QCheckBox(QStringLiteral("Host Filesystem Access (Disabled)"), capGroup);
    m_capFilesystemCheck->setChecked(false);
    m_capFilesystemCheck->setEnabled(false);
    m_capFilesystemCheck->setToolTip(
        QStringLiteral("Wallpaper packages cannot read or write outside their sandbox."));
    m_capFilesystemCheck->setStyleSheet(QStringLiteral("color: #94a3b8;"));
    capLayout->addWidget(m_capFilesystemCheck, 3, 1);

    layout->addWidget(capGroup);

    // Issues & Repairs Tree
    auto *issuesGroup = new QGroupBox(QStringLiteral("Compatibility Notes"), this);
    auto *issuesLayout = new QVBoxLayout(issuesGroup);
    issuesLayout->setSpacing(8);

    m_issuesTree = new QTreeWidget(issuesGroup);
    m_issuesTree->setColumnCount(4);
    m_issuesTree->setHeaderLabels({QStringLiteral("Severity"), QStringLiteral("File"),
                                   QStringLiteral("Message"), QStringLiteral("Suggested Fix")});
    m_issuesTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_issuesTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_issuesTree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_issuesTree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    issuesLayout->addWidget(m_issuesTree, 1);

    auto *repairRow = new QHBoxLayout();
    repairRow->setSpacing(8);
    m_repairsSummaryLabel = new QLabel(QStringLiteral("No issues detected."), issuesGroup);
    m_repairsSummaryLabel->setStyleSheet(QStringLiteral("color: #94a3b8; font-size: 11px;"));
    repairRow->addWidget(m_repairsSummaryLabel, 1);

    m_applyRepairsBtn = new QPushButton(QStringLiteral("Apply Suggested Fixes"), issuesGroup);
    m_applyRepairsBtn->setEnabled(false);
    repairRow->addWidget(m_applyRepairsBtn);

    issuesLayout->addLayout(repairRow);
    layout->addWidget(issuesGroup, 1);

    connect(m_applyRepairsBtn, &QPushButton::clicked, this, &CapabilityAuditPage::applyAutoRepairs);
}

void CapabilityAuditPage::initializePage() { runCompatibilityAudit(); }

void CapabilityAuditPage::runCompatibilityAudit()
{
    if (!m_ctx || m_ctx->stagingDir.isEmpty())
        return;

    m_issuesTree->clear();
    m_ctx->issues.clear();
    m_ctx->repairs.clear();

    QDir stg(m_ctx->stagingDir);
    bool blockedByBinary = false;
    bool detectedAudioUniforms = false;

    /* Reject disguised ELF/PE executables in any package type */
    QDirIterator binIt(m_ctx->stagingDir, QDir::Files | QDir::NoDotAndDotDot,
                       QDirIterator::Subdirectories);
    while (binIt.hasNext()) {
        const QString fPath = binIt.next();
        if (isDisguisedExecutable(fPath)) {
            blockedByBinary = true;
            m_ctx->issues.append(
                {QStringLiteral("SECURITY_BINARY_PAYLOAD"), WE_ISSUE_SEV_FATAL,
                 QStringLiteral("Native executable (ELF/PE) binary detected in staging directory."),
                 stg.relativeFilePath(fPath), 0,
                 QStringLiteral("Remove native executables from wallpaper assets.")});
        }
    }

    if (blockedByBinary) {
        m_ctx->compatTier = WE_TIER_4_BLOCKED;
        m_ctx->compatScore = 0;
        m_ctx->estimatedVramMb = 0.0f;
        m_ctx->estimatedFps = 0;
    } else if (m_ctx->projectType == WWPKG_TYPE_SHADER_FRAGMENT ||
               m_ctx->projectType == WWPKG_TYPE_SHADER_COMPUTE) {
        const bool isCompute = (m_ctx->projectType == WWPKG_TYPE_SHADER_COMPUTE);
        QString entryPath = stg.filePath(m_ctx->entrypointPipeline);
        bool graphAuditFailed = false;
        if (m_ctx->entrypointPipeline.endsWith(QStringLiteral(".wwgraph"), Qt::CaseInsensitive)) {
            QByteArray glslBytes;
            QString graphTitle;
            QString graphAuthor;
            QString graphErr;
            bool forceCompute = false;
            if (compileWwgraphFileToGlsl(entryPath, glslBytes, graphTitle, graphAuthor,
                                         forceCompute, graphErr)) {
                const QString compiledRel = QStringLiteral("shaders/main.frag");
                const QString compiledDst = stg.filePath(compiledRel);
                QFileInfo(compiledDst).dir().mkpath(QStringLiteral("."));
                QFile outFrag(compiledDst);
                if (outFrag.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    outFrag.write(glslBytes);
                    outFrag.close();
                    m_ctx->entrypointPipeline = compiledRel;
                    entryPath = compiledDst;
                }
            } else {
                graphAuditFailed = true;
                m_ctx->compatTier = WE_TIER_4_BLOCKED;
                m_ctx->compatScore = 0;
                m_ctx->estimatedVramMb = 0.0f;
                m_ctx->estimatedFps = 0;
                m_ctx->issues.append(
                    {QStringLiteral("SHADER_GRAPH_INVALID"), WE_ISSUE_SEV_FATAL, graphErr,
                     m_ctx->entrypointPipeline, 0,
                     QStringLiteral("Resolve cycle or pin errors in Graph Editor.")});
            }
        }
        if (!graphAuditFailed) {
            const ShaderAuditResult shaderRes =
                auditShaderSource(entryPath, m_ctx->entrypointPipeline, isCompute);

            m_ctx->compatTier = shaderRes.tier;
            m_ctx->compatScore = shaderRes.score;
            m_ctx->issues.append(shaderRes.issues);
            detectedAudioUniforms = shaderRes.usesAudioUniforms;

            float vramEst = isCompute ? 64.0f : 48.0f;
            waywal_shader_meta_t meta;
            waywal_shader_meta_init(&meta);
            if (QFile::exists(entryPath) &&
                waywal_shader_parse_file(entryPath.toUtf8().constData(), &meta) &&
                meta.has_multi_pass) {
                vramEst += 32.0f;
            }
            m_ctx->estimatedVramMb = vramEst;
            m_ctx->estimatedFps = m_ctx->targetFps ? m_ctx->targetFps : 60;
        }
    } else if (m_ctx->projectType == WWPKG_TYPE_VIDEO_STREAM) {
        const QString entryPath = stg.filePath(m_ctx->entrypointPipeline);
        if (!QFileInfo::exists(entryPath)) {
            m_ctx->compatTier = WE_TIER_4_BLOCKED;
            m_ctx->compatScore = 0;
            m_ctx->estimatedVramMb = 0.0f;
            m_ctx->estimatedFps = 0;
            m_ctx->issues.append({QStringLiteral("VIDEO_ENTRYPOINT_MISSING"), WE_ISSUE_SEV_FATAL,
                                  QStringLiteral("Staged video entrypoint '%1' does not exist.")
                                      .arg(m_ctx->entrypointPipeline),
                                  m_ctx->entrypointPipeline, 0,
                                  QStringLiteral("Verify the source video file is readable.")});
        } else if (!hasValidVideoContainerHeader(entryPath)) {
            m_ctx->compatTier = WE_TIER_4_BLOCKED;
            m_ctx->compatScore = 0;
            m_ctx->estimatedVramMb = 0.0f;
            m_ctx->estimatedFps = 0;
            m_ctx->issues.append(
                {QStringLiteral("VIDEO_HEADER_INVALID"), WE_ISSUE_SEV_FATAL,
                 QStringLiteral(
                     "Video stream header is missing a valid MP4/WebM/MKV/Ogg signature."),
                 m_ctx->entrypointPipeline, 0,
                 QStringLiteral("Provide a valid H.264/HEVC/AV1/VP9 video container.")});
        } else {
            m_ctx->compatTier = WE_TIER_1_NATIVE;
            m_ctx->compatScore = 100;
            m_ctx->estimatedVramMb = 64.0f;
            m_ctx->estimatedFps = m_ctx->targetFps ? m_ctx->targetFps : 60;
        }
    } else {
        we_compat_scanner_t *scanner = we_compat_scanner_create(true, true);
        if (!scanner)
            return;

        we_compat_report_t *report = we_compat_report_alloc();
        if (!report) {
            we_compat_scanner_destroy(scanner);
            return;
        }

        const bool scanned =
            we_compat_scan_directory(scanner, m_ctx->stagingDir.toUtf8().constData(), report);

        if (scanned && report->tier != WE_TIER_UNKNOWN) {
            m_ctx->compatTier = report->tier;
            m_ctx->compatScore = report->compatibility_score;
            m_ctx->estimatedVramMb = report->summary.estimated_vram_mb;
            m_ctx->estimatedFps = report->summary.estimated_fps;

            for (uint32_t i = 0; i < report->issue_count; i++) {
                AuditIssueEntry issue;
                issue.code = QString::fromUtf8(report->issues[i].code);
                issue.severity = static_cast<int>(report->issues[i].severity);
                issue.message = QString::fromUtf8(report->issues[i].message);
                issue.filePath = QString::fromUtf8(report->issues[i].file_path);
                issue.lineNumber = report->issues[i].line_number;
                issue.suggestedFix = QString::fromUtf8(report->issues[i].suggested_fix);
                m_ctx->issues.append(issue);
            }

            for (uint32_t i = 0; i < report->repair_count; i++) {
                AuditRepairEntry repair;
                repair.transformationId = QString::fromUtf8(report->repairs[i].transformation_id);
                repair.targetFile = QString::fromUtf8(report->repairs[i].target_file);
                repair.description = QString::fromUtf8(report->repairs[i].description);
                m_ctx->repairs.append(repair);
            }
        } else {
            m_ctx->compatTier = WE_TIER_4_BLOCKED;
            m_ctx->compatScore = 0;
            m_ctx->estimatedVramMb = 0.0f;
            m_ctx->estimatedFps = 0;
            m_ctx->issues.append({QStringLiteral("SCENE_SCAN_FAILED"), WE_ISSUE_SEV_FATAL,
                                  QStringLiteral("Scene compatibility audit failed."),
                                  m_ctx->entrypointPipeline, 0,
                                  QStringLiteral("Verify scene.json and project assets.")});
        }

        we_compat_report_free(report);
        we_compat_scanner_destroy(scanner);
    }

    // Update UI Badges
    switch (m_ctx->compatTier) {
    case WE_TIER_1_NATIVE:
        m_tierBadge->setText(QStringLiteral("Native"));
        m_tierBadge->setStyleSheet(
            QStringLiteral("background-color: #10b981; color: white; font-weight: bold; "
                           "padding: 6px 12px; border-radius: 4px; font-size: 13px;"));
        break;
    case WE_TIER_2_TRANSPILED:
        m_tierBadge->setText(QStringLiteral("Transpiled"));
        m_tierBadge->setStyleSheet(
            QStringLiteral("background-color: #06b6d4; color: white; font-weight: bold; "
                           "padding: 6px 12px; border-radius: 4px; font-size: 13px;"));
        break;
    case WE_TIER_3_DEGRADED:
        m_tierBadge->setText(QStringLiteral("Limited"));
        m_tierBadge->setStyleSheet(
            QStringLiteral("background-color: #f59e0b; color: white; font-weight: bold; "
                           "padding: 6px 12px; border-radius: 4px; font-size: 13px;"));
        break;
    case WE_TIER_4_BLOCKED:
    default:
        m_tierBadge->setText(QStringLiteral("Incompatible"));
        m_tierBadge->setStyleSheet(
            QStringLiteral("background-color: #ef4444; color: white; font-weight: bold; "
                           "padding: 6px 12px; border-radius: 4px; font-size: 13px;"));
        break;
    }

    m_scoreLabel->setText(QStringLiteral("Compatibility Score: %1/100").arg(m_ctx->compatScore));
    m_scoreBar->setValue(m_ctx->compatScore);
    m_metricsLabel->setText(QStringLiteral("Estimated VRAM: %1 MB | Target FPS: %2 | Notes: %3")
                                .arg(QString::number(m_ctx->estimatedVramMb, 'f', 1))
                                .arg(m_ctx->estimatedFps)
                                .arg(m_ctx->issues.size()));

    // Populate issues tree
    for (const auto &iss : m_ctx->issues) {
        auto *item = new QTreeWidgetItem(m_issuesTree);
        QString sevStr;
        QColor sevColor;
        switch (iss.severity) {
        case WE_ISSUE_SEV_INFO:
            sevStr = QStringLiteral("INFO");
            sevColor = QColor(QStringLiteral("#38bdf8"));
            break;
        case WE_ISSUE_SEV_WARNING:
            sevStr = QStringLiteral("WARNING");
            sevColor = QColor(QStringLiteral("#fbbf24"));
            break;
        case WE_ISSUE_SEV_ERROR:
            sevStr = QStringLiteral("ERROR");
            sevColor = QColor(QStringLiteral("#f87171"));
            break;
        case WE_ISSUE_SEV_FATAL:
            sevStr = QStringLiteral("FATAL");
            sevColor = QColor(QStringLiteral("#ef4444"));
            break;
        default:
            sevStr = QStringLiteral("UNKNOWN");
            sevColor = QColor(QStringLiteral("#94a3b8"));
            break;
        }
        item->setText(0, sevStr);
        item->setForeground(0, QBrush(sevColor));
        item->setText(1, iss.filePath);
        item->setText(2, iss.message);
        item->setText(3, iss.suggestedFix.isEmpty() ? QStringLiteral("None") : iss.suggestedFix);
    }

    if (m_ctx->projectType == WWPKG_TYPE_SHADER_COMPUTE) {
        m_capComputeCheck->setChecked(true);
    }
    if (detectedAudioUniforms) {
        m_capAudioSpectrumCheck->setChecked(true);
    }
    if (stg.exists(QStringLiteral("scripts/main.js"))) {
        m_capScriptingCheck->setChecked(true);
    }

    if (!m_ctx->repairs.isEmpty()) {
        m_repairsSummaryLabel->setText(
            QStringLiteral("%1 automatic fix(es) available.").arg(m_ctx->repairs.size()));
        m_applyRepairsBtn->setEnabled(true);
    } else {
        m_repairsSummaryLabel->setText(
            m_ctx->issues.isEmpty()
                ? QStringLiteral("No issues detected.")
                : QStringLiteral("%1 compatibility note(s) recorded.").arg(m_ctx->issues.size()));
        m_applyRepairsBtn->setEnabled(false);
    }

    emit completeChanged();
}

void CapabilityAuditPage::applyAutoRepairs()
{
    if (!m_ctx || m_ctx->stagingDir.isEmpty() || !isScenePackageType(m_ctx->projectType))
        return;

    we_compat_report_t *report = we_compat_report_alloc();
    if (!report)
        return;

    we_compat_scanner_t *scanner = we_compat_scanner_create(true, true);
    if (scanner) {
        if (we_compat_scan_directory(scanner, m_ctx->stagingDir.toUtf8().constData(), report)) {
            we_compat_apply_auto_repairs(report, m_ctx->stagingDir.toUtf8().constData(),
                                         m_ctx->stagingDir.toUtf8().constData());
        }
        we_compat_scanner_destroy(scanner);
    }
    we_compat_report_free(report);

    runCompatibilityAudit();
}

bool CapabilityAuditPage::isComplete() const
{
    return m_ctx && m_ctx->compatTier != WE_TIER_4_BLOCKED;
}

bool CapabilityAuditPage::validatePage()
{
    if (!isComplete()) {
        QMessageBox::critical(
            this, QStringLiteral("Cannot Package Wallpaper"),
            QStringLiteral("This wallpaper has fatal compatibility or security errors "
                           "and cannot be packaged."));
        return false;
    }

    m_ctx->capComputeShaders = m_capComputeCheck->isChecked();
    m_ctx->capAudioSpectrum = m_capAudioSpectrumCheck->isChecked();
    m_ctx->capAudioPlayback = m_capAudioPlaybackCheck->isChecked();
    m_ctx->capScripting = m_capScriptingCheck->isChecked();
    m_ctx->capForeignToplevel = m_capForeignToplevelCheck->isChecked();

    return true;
}

/* ========================================================================= */
/* Step 5: CompilationSigningPage Implementation                             */
/* ========================================================================= */

CompilationSigningPage::CompilationSigningPage(CreatorPackagingContext *ctx, QWidget *parent)
    : QWizardPage(parent), m_ctx(ctx)
{
    setTitle(QStringLiteral("Step 5: Sign & Build"));
    setSubTitle(
        QStringLiteral("Sign the package with your author key and build the .wwpkg archive."));
    setupUi();
}

void CompilationSigningPage::setupUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(12);

    // Cryptographic Identity Group
    auto *keyGroup = new QGroupBox(QStringLiteral("Signing Key (Ed25519)"), this);
    auto *keyLayout = new QVBoxLayout(keyGroup);
    keyLayout->setSpacing(8);

    auto *keyRow = new QHBoxLayout();
    keyRow->setSpacing(8);
    m_keyPathEdit = new QLineEdit(keyGroup);
    keyRow->addWidget(m_keyPathEdit, 1);

    m_browseKeyBtn = new QPushButton(QStringLiteral("Browse Key..."), keyGroup);
    keyRow->addWidget(m_browseKeyBtn);

    m_generateKeyBtn = new QPushButton(QStringLiteral("Generate Keypair"), keyGroup);
    m_generateKeyBtn->setStyleSheet(
        QStringLiteral("background-color: #0284c7; color: white; font-weight: bold;"));
    keyRow->addWidget(m_generateKeyBtn);

    keyLayout->addLayout(keyRow);

    m_keyFingerprintLabel = new QLabel(QStringLiteral("Public Key: None loaded"), keyGroup);
    m_keyFingerprintLabel->setStyleSheet(
        QStringLiteral("font-family: monospace; color: #38bdf8; font-size: 11px;"));
    keyLayout->addWidget(m_keyFingerprintLabel);

    layout->addWidget(keyGroup);

    // Output & Build Action Group
    auto *buildGroup = new QGroupBox(QStringLiteral("Build Package"), this);
    auto *buildLayout = new QVBoxLayout(buildGroup);
    buildLayout->setSpacing(10);

    auto *outRow = new QHBoxLayout();
    outRow->setSpacing(8);
    auto *outLbl = new QLabel(QStringLiteral("Output File:"), buildGroup);
    outRow->addWidget(outLbl);

    m_outPkgPathEdit = new QLineEdit(buildGroup);
    outRow->addWidget(m_outPkgPathEdit, 1);

    m_browseOutBtn = new QPushButton(QStringLiteral("Browse..."), buildGroup);
    outRow->addWidget(m_browseOutBtn);
    buildLayout->addLayout(outRow);

    auto *actRow = new QHBoxLayout();
    actRow->setSpacing(12);
    m_compileBtn = new QPushButton(QStringLiteral("Build & Sign Package"), buildGroup);
    m_compileBtn->setFixedHeight(32);
    m_compileBtn->setStyleSheet(
        QStringLiteral("background-color: #059669; color: white; font-weight: bold; font-size: "
                       "12px; padding: 6px 16px;"));
    actRow->addWidget(m_compileBtn);

    m_verificationBadge = new QLabel(QStringLiteral("Status: Not Built"), buildGroup);
    m_verificationBadge->setStyleSheet(QStringLiteral("color: #94a3b8; font-weight: bold;"));
    actRow->addWidget(m_verificationBadge);
    actRow->addStretch();
    buildLayout->addLayout(actRow);

    m_progressBar = new QProgressBar(buildGroup);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setFixedHeight(12);
    buildLayout->addWidget(m_progressBar);

    layout->addWidget(buildGroup);

    // Compilation Log
    auto *logGroup = new QGroupBox(QStringLiteral("Build Log"), this);
    auto *logLayout = new QVBoxLayout(logGroup);
    m_logEdit = new QTextEdit(logGroup);
    m_logEdit->setReadOnly(true);
    m_logEdit->setStyleSheet(QStringLiteral(
        "background-color: #12141a; font-family: monospace; font-size: 11px; color: #e2e8f0;"));
    logLayout->addWidget(m_logEdit);
    layout->addWidget(logGroup, 1);

    // Wiring
    connect(m_generateKeyBtn, &QPushButton::clicked, this,
            &CompilationSigningPage::generateKeypair);
    connect(m_browseKeyBtn, &QPushButton::clicked, this, &CompilationSigningPage::browseKeyFile);
    connect(m_browseOutBtn, &QPushButton::clicked, this, [this]() {
        QString f = QFileDialog::getSaveFileName(this, QStringLiteral("Save Package"),
                                                 m_outPkgPathEdit->text(),
                                                 QStringLiteral("WayWal Package (*.wwpkg)"));
        if (!f.isEmpty()) {
            m_outPkgPathEdit->setText(f);
        }
    });
    connect(m_compileBtn, &QPushButton::clicked, this,
            &CompilationSigningPage::compileAndSignPackage);
}

void CompilationSigningPage::initializePage()
{
    if (!m_ctx)
        return;

    if (m_keyPathEdit->text().isEmpty()) {
        m_keyPathEdit->setText(QDir::homePath() +
                               QStringLiteral("/.config/waywal/keys/author_ed25519.key"));
    }

    if (m_outPkgPathEdit->text().isEmpty()) {
        QString outDir = QDir::homePath() + QStringLiteral("/waywal_packages");
        QDir().mkpath(outDir);
        m_outPkgPathEdit->setText(
            outDir + QStringLiteral("/%1-%2.wwpkg").arg(m_ctx->slug).arg(m_ctx->version));
    }

    checkOrLoadKey();
}

void CompilationSigningPage::checkOrLoadKey()
{
    QString keyPath = m_keyPathEdit->text().trimmed();
    QFile keyFile(keyPath);

    if (!keyFile.exists() || !keyFile.open(QIODevice::ReadOnly)) {
        m_keyFingerprintLabel->setText(QStringLiteral(
            "Key file not found. Click 'Generate Keypair' to create a signing key."));
        m_keyFingerprintLabel->setStyleSheet(QStringLiteral("color: #f59e0b;"));
        wipeByteArray(m_ctx->authorPrivKey);
        m_ctx->authorPubKey.clear();
        return;
    }

    QByteArray raw = keyFile.readAll();
    keyFile.close();

    QByteArray trimmed = raw.trimmed();
    uint8_t pk[32] = {0};
    uint8_t sk[64] = {0};
    bool keyValid = false;

    if (trimmed.size() == 128 && isAsciiHex(trimmed)) {
        QByteArray decoded = QByteArray::fromHex(trimmed);
        if (decoded.size() == 64 &&
            wwpkg_ed25519_seed_keypair(
                pk, sk, reinterpret_cast<const uint8_t *>(decoded.constData())) == 0) {
            bool tailZero = true;
            for (int i = 32; i < 64; ++i) {
                if (decoded[i] != '\0') {
                    tailZero = false;
                    break;
                }
            }
            if (tailZero || memcmp(decoded.constData() + 32, pk, 32) == 0) {
                keyValid = true;
            }
        }
        wipeByteArray(decoded);
    } else if (trimmed.size() == 64 && isAsciiHex(trimmed)) {
        QByteArray seed = QByteArray::fromHex(trimmed);
        if (seed.size() == 32 &&
            wwpkg_ed25519_seed_keypair(pk, sk,
                                       reinterpret_cast<const uint8_t *>(seed.constData())) == 0) {
            keyValid = true;
        }
        wipeByteArray(seed);
    } else if (raw.size() == 64) {
        if (wwpkg_ed25519_seed_keypair(pk, sk,
                                       reinterpret_cast<const uint8_t *>(raw.constData())) == 0) {
            bool tailZero = true;
            for (int i = 32; i < 64; ++i) {
                if (raw[i] != '\0') {
                    tailZero = false;
                    break;
                }
            }
            if (tailZero || memcmp(raw.constData() + 32, pk, 32) == 0) {
                keyValid = true;
            }
        }
    } else if (raw.size() == 32) {
        if (wwpkg_ed25519_seed_keypair(pk, sk,
                                       reinterpret_cast<const uint8_t *>(raw.constData())) == 0) {
            keyValid = true;
        }
    }

    wipeByteArray(raw);
    wipeByteArray(trimmed);

    if (keyValid) {
        wipeByteArray(m_ctx->authorPrivKey);
        m_ctx->authorPrivKey = QByteArray(reinterpret_cast<const char *>(sk), 64);
        m_ctx->authorPubKey = QByteArray(reinterpret_cast<const char *>(pk), 32);
        const QString pubHex = QString::fromLatin1(m_ctx->authorPubKey.toHex());
        m_keyFingerprintLabel->setText(QStringLiteral("Public Key: %1").arg(pubHex));
        m_keyFingerprintLabel->setStyleSheet(
            QStringLiteral("font-family: monospace; color: #38bdf8; font-weight: bold;"));
        addPubkeyToTrustedKeys(pubHex);
    } else {
        wipeByteArray(m_ctx->authorPrivKey);
        m_ctx->authorPubKey.clear();
        m_keyFingerprintLabel->setText(QStringLiteral(
            "Invalid key format. Expected a 32-byte seed or 64-byte Ed25519 private key."));
        m_keyFingerprintLabel->setStyleSheet(QStringLiteral("color: #ef4444;"));
    }

    explicit_bzero(sk, sizeof(sk));
}

void CompilationSigningPage::generateKeypair()
{
    uint8_t pub[32] = {0};
    uint8_t priv[64] = {0};
    wwpkg_generate_keypair(pub, priv);
    bool pubNonZero = false;
    for (uint8_t b : pub) {
        if (b != 0) {
            pubNonZero = true;
            break;
        }
    }
    if (!pubNonZero) {
        explicit_bzero(priv, sizeof(priv));
        m_keyFingerprintLabel->setText(
            QStringLiteral("Failed to generate Ed25519 keypair from OS CSPRNG."));
        m_keyFingerprintLabel->setStyleSheet(QStringLiteral("color: #ef4444;"));
        return;
    }

    QString keyPath = m_keyPathEdit->text().trimmed();
    if (keyPath.isEmpty()) {
        keyPath = QDir::homePath() + QStringLiteral("/.config/waywal/keys/author_ed25519.key");
        m_keyPathEdit->setText(keyPath);
    }

    QFileInfo fi(keyPath);
    const QString keyDirPath = fi.dir().absolutePath();
    fi.dir().mkpath(QStringLiteral("."));
    (void)::chmod(keyDirPath.toUtf8().constData(), 0700);

    const QByteArray keyPathUtf8 = keyPath.toUtf8();
    const int fd = ::open(keyPathUtf8.constData(),
                          O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        explicit_bzero(priv, sizeof(priv));
        m_keyFingerprintLabel->setText(QStringLiteral(
            "Refused or failed to open key path securely (check permissions/symlinks)."));
        m_keyFingerprintLabel->setStyleSheet(QStringLiteral("color: #ef4444;"));
        return;
    }

    (void)::fchmod(fd, 0600);
    QByteArray hex = QByteArray(reinterpret_cast<const char *>(priv), 64).toHex() + '\n';
    const bool wroteOk = writeAllFd(fd, hex.constData(), static_cast<size_t>(hex.size()));
    wipeByteArray(hex);
    (void)::close(fd);

    if (!wroteOk) {
        (void)::unlink(keyPathUtf8.constData());
        explicit_bzero(priv, sizeof(priv));
        m_keyFingerprintLabel->setText(QStringLiteral("Failed to write Ed25519 key file."));
        m_keyFingerprintLabel->setStyleSheet(QStringLiteral("color: #ef4444;"));
        return;
    }

    wipeByteArray(m_ctx->authorPrivKey);
    m_ctx->authorPrivKey = QByteArray(reinterpret_cast<const char *>(priv), 64);
    m_ctx->authorPubKey = QByteArray(reinterpret_cast<const char *>(pub), 32);
    explicit_bzero(priv, sizeof(priv));

    const QString pubHex = QString::fromLatin1(m_ctx->authorPubKey.toHex());
    m_keyFingerprintLabel->setText(QStringLiteral("Generated Public Key: %1").arg(pubHex));
    m_keyFingerprintLabel->setStyleSheet(
        QStringLiteral("font-family: monospace; color: #10b981; font-weight: bold;"));

    addPubkeyToTrustedKeys(pubHex);
}

void CompilationSigningPage::browseKeyFile()
{
    QString f = QFileDialog::getOpenFileName(
        this, QStringLiteral("Select Author Key"), m_keyPathEdit->text(),
        QStringLiteral("Key Files (*.key *.priv *.pem);;All Files (*)"));
    if (!f.isEmpty()) {
        m_keyPathEdit->setText(f);
        checkOrLoadKey();
    }
}

CompilationSigningPage::~CompilationSigningPage()
{
    if (m_buildThread.joinable()) {
        m_buildThread.join();
    }
}

void CompilationSigningPage::setBuildUiBusy(bool busy)
{
    m_building.store(busy, std::memory_order_release);
    if (m_compileBtn) {
        m_compileBtn->setEnabled(!busy);
    }
    if (m_browseKeyBtn) {
        m_browseKeyBtn->setEnabled(!busy);
    }
    if (m_generateKeyBtn) {
        m_generateKeyBtn->setEnabled(!busy);
    }
    if (m_browseOutBtn) {
        m_browseOutBtn->setEnabled(!busy);
    }
    if (m_keyPathEdit) {
        m_keyPathEdit->setEnabled(!busy);
    }
    if (m_outPkgPathEdit) {
        m_outPkgPathEdit->setEnabled(!busy);
    }
    if (auto *wiz = wizard()) {
        if (auto *backBtn = wiz->button(QWizard::BackButton)) {
            backBtn->setEnabled(!busy);
        }
        if (auto *cancelBtn = wiz->button(QWizard::CancelButton)) {
            cancelBtn->setEnabled(!busy);
        }
    }
    emit completeChanged();
}

void CompilationSigningPage::compileAndSignPackage()
{
    if (!m_ctx || m_ctx->stagingDir.isEmpty() || m_building.load(std::memory_order_acquire))
        return;

    if (m_ctx->authorPrivKey.size() != 64) {
        generateKeypair();
        if (m_ctx->authorPrivKey.size() != 64) {
            return;
        }
    }

    QString outPkg = m_outPkgPathEdit->text().trimmed();
    if (outPkg.isEmpty()) {
        outPkg =
            QDir::homePath() +
            QStringLiteral("/waywal_packages/%1-%2.wwpkg").arg(m_ctx->slug).arg(m_ctx->version);
        m_outPkgPathEdit->setText(outPkg);
    }

    QFileInfo outFi(outPkg);
    outFi.dir().mkpath(QStringLiteral("."));

    m_ctx->isCompiled = false;
    m_ctx->isVerified = false;
    m_logEdit->clear();
    m_progressBar->setValue(10);
    m_verificationBadge->setText(QStringLiteral("Status: Building..."));
    m_verificationBadge->setStyleSheet(QStringLiteral("color: #38bdf8; font-weight: bold;"));
    m_logEdit->append(QStringLiteral("[%1] Starting package build...")
                          .arg(QDateTime::currentDateTime().toString(Qt::ISODate)));
    m_logEdit->append(QStringLiteral("Staging Directory: %1").arg(m_ctx->stagingDir));
    m_logEdit->append(QStringLiteral("Output File: %1").arg(outPkg));

    // Construct wwpkg_manifest_t
    wwpkg_manifest_t manifest;
    memset(&manifest, 0, sizeof(manifest));

    strncpy(manifest.schema_version, "wwpkg/v2", sizeof(manifest.schema_version) - 1);
    strncpy(manifest.id, m_ctx->packageId.toUtf8().constData(), sizeof(manifest.id) - 1);
    strncpy(manifest.slug, m_ctx->slug.toUtf8().constData(), sizeof(manifest.slug) - 1);
    strncpy(manifest.name, m_ctx->name.toUtf8().constData(), sizeof(manifest.name) - 1);
    strncpy(manifest.version, m_ctx->version.toUtf8().constData(), sizeof(manifest.version) - 1);
    strncpy(manifest.author_name, m_ctx->authorName.toUtf8().constData(),
            sizeof(manifest.author_name) - 1);
    strncpy(manifest.author_email, m_ctx->authorEmail.toUtf8().constData(),
            sizeof(manifest.author_email) - 1);
    memcpy(manifest.author_pubkey, m_ctx->authorPubKey.constData(), 32);
    strncpy(manifest.license, m_ctx->license.toUtf8().constData(), sizeof(manifest.license) - 1);
    strncpy(manifest.description, m_ctx->description.toUtf8().constData(),
            sizeof(manifest.description) - 1);

    manifest.tag_count =
        static_cast<uint32_t>(qMin(m_ctx->tags.size(), static_cast<qsizetype>(WWPKG_MAX_TAGS)));
    for (uint32_t i = 0; i < manifest.tag_count; i++) {
        strncpy(manifest.tags[i], m_ctx->tags[i].toUtf8().constData(),
                sizeof(manifest.tags[i]) - 1);
    }

    manifest.type = m_ctx->projectType;
    strncpy(manifest.entrypoint_pipeline, m_ctx->entrypointPipeline.toUtf8().constData(),
            sizeof(manifest.entrypoint_pipeline) - 1);
    strncpy(manifest.entrypoint_preview, "preview.webp", sizeof(manifest.entrypoint_preview) - 1);
    strncpy(manifest.entrypoint_audio, m_ctx->entrypointAudio.toUtf8().constData(),
            sizeof(manifest.entrypoint_audio) - 1);

    strncpy(manifest.min_wwal_version, "1.0.0", sizeof(manifest.min_wwal_version) - 1);
    manifest.abi_version = 1;
    strncpy(manifest.gl_profile, "gles_31", sizeof(manifest.gl_profile) - 1);

    manifest.capabilities.compute_shaders = m_ctx->capComputeShaders;
    manifest.capabilities.audio_spectrum = m_ctx->capAudioSpectrum;
    manifest.capabilities.audio_playback = m_ctx->capAudioPlayback;
    manifest.capabilities.scripting = m_ctx->capScripting;
    manifest.capabilities.foreign_toplevel = m_ctx->capForeignToplevel;
    manifest.capabilities.network = false;    // Air-gap invariant
    manifest.capabilities.filesystem = false; // Air-gap invariant

    manifest.budgets.max_vram_mb = m_ctx->maxVramMb;
    manifest.budgets.target_fps = m_ctx->targetFps;

    m_progressBar->setValue(35);
    m_logEdit->append(QStringLiteral("Writing manifest.json and compressing Zstd archive..."));

    if (m_buildThread.joinable()) {
        m_buildThread.join();
    }
    setBuildUiBusy(true);

    const QByteArray stagingUtf8 = m_ctx->stagingDir.toUtf8();
    const QByteArray outPkgUtf8 = outPkg.toUtf8();
    QByteArray privKeyCopy = m_ctx->authorPrivKey;
    QPointer<CompilationSigningPage> safeSelf(this);

    m_buildThread = std::jthread([safeSelf, manifest, stagingUtf8, outPkgUtf8, outPkg,
                                  privKeyCopy = std::move(privKeyCopy)]() mutable {
        struct BuildOutcome {
            bool buildOk{false};
            int errCode{0};
            bool openedPkg{false};
            bool verified{false};
            bool isTrusted{false};
            QByteArray merkleRoot;
            QByteArray signature;
        } res;

        res.buildOk = wwpkg_build_package(
            stagingUtf8.constData(), &manifest,
            reinterpret_cast<const uint8_t *>(privKeyCopy.constData()), outPkgUtf8.constData());
        wipeByteArray(privKeyCopy);

        if (!res.buildOk) {
            res.errCode = wwpkg_get_last_error(nullptr);
        } else {
            wwpkg_package_t *pkg = wwpkg_open(outPkgUtf8.constData());
            if (pkg) {
                res.openedPkg = true;
                res.verified = wwpkg_verify_signature(pkg, &res.isTrusted);
                const wwpkg_signature_t *sig = wwpkg_get_signature(pkg);
                if (sig) {
                    res.merkleRoot =
                        QByteArray(reinterpret_cast<const char *>(sig->merkle_root), 32);
                    res.signature = QByteArray(reinterpret_cast<const char *>(sig->signature), 64);
                }
                wwpkg_close(pkg);
            }
        }

        if (!qApp) {
            return;
        }
        QMetaObject::invokeMethod(
            qApp,
            [safeSelf, outPkg, res = std::move(res)]() {
                if (!safeSelf || !safeSelf->m_ctx) {
                    return;
                }
                safeSelf->setBuildUiBusy(false);

                if (!res.buildOk) {
                    safeSelf->m_logEdit->append(
                        QStringLiteral("[ERROR] Failed to build package: %1")
                            .arg(QString::fromUtf8(wwpkg_error_str(res.errCode))));
                    safeSelf->m_verificationBadge->setText(QStringLiteral("Status: Build Failed"));
                    safeSelf->m_verificationBadge->setStyleSheet(
                        QStringLiteral("color: #ef4444; font-weight: bold;"));
                    safeSelf->m_progressBar->setValue(0);
                    return;
                }

                safeSelf->m_progressBar->setValue(75);
                safeSelf->m_logEdit->append(
                    QStringLiteral("[OK] Archive compressed with Zstandard (level 12)."));
                safeSelf->m_logEdit->append(QStringLiteral("Verifying package signature..."));

                if (!res.openedPkg) {
                    safeSelf->m_logEdit->append(
                        QStringLiteral("[ERROR] Could not open newly built package."));
                    safeSelf->m_verificationBadge->setText(
                        QStringLiteral("Status: Verification Failed"));
                    safeSelf->m_verificationBadge->setStyleSheet(
                        QStringLiteral("color: #ef4444; font-weight: bold;"));
                    return;
                }

                safeSelf->m_ctx->merkleRoot = res.merkleRoot;
                safeSelf->m_ctx->signature = res.signature;
                safeSelf->m_progressBar->setValue(100);

                if (res.verified) {
                    safeSelf->m_ctx->isCompiled = true;
                    safeSelf->m_ctx->isVerified = true;
                    safeSelf->m_ctx->isTrusted = res.isTrusted;
                    safeSelf->m_ctx->outPkgPath = outPkg;
                    safeSelf->m_ctx->compiledSizeBytes = QFileInfo(outPkg).size();

                    safeSelf->m_logEdit->append(
                        QStringLiteral("[OK] Merkle Root: %1")
                            .arg(QString::fromLatin1(safeSelf->m_ctx->merkleRoot.toHex())));
                    safeSelf->m_logEdit->append(
                        QStringLiteral("[OK] Ed25519 Signature: %1...")
                            .arg(QString::fromLatin1(safeSelf->m_ctx->signature.toHex().left(32))));
                    safeSelf->m_logEdit->append(QStringLiteral("[OK] Key Trust Status: %1")
                                                    .arg(res.isTrusted
                                                             ? QStringLiteral("Trusted")
                                                             : QStringLiteral("Untrusted")));
                    safeSelf->m_logEdit->append(
                        QStringLiteral("[OK] Package built and verified: %1 (%2)")
                            .arg(outPkg)
                            .arg(formatBytes(safeSelf->m_ctx->compiledSizeBytes)));

                    safeSelf->m_verificationBadge->setText(QStringLiteral("Signed & Verified"));
                    safeSelf->m_verificationBadge->setStyleSheet(
                        QStringLiteral("color: #10b981; font-weight: bold;"));

                    emit safeSelf->completeChanged();
                } else {
                    safeSelf->m_logEdit->append(
                        QStringLiteral("[ERROR] Signature verification failed."));
                    safeSelf->m_verificationBadge->setText(
                        QStringLiteral("Status: Verification Failed"));
                    safeSelf->m_verificationBadge->setStyleSheet(
                        QStringLiteral("color: #ef4444; font-weight: bold;"));
                }
            },
            Qt::QueuedConnection);
    });
}

bool CompilationSigningPage::isComplete() const
{
    return m_ctx && !m_building.load(std::memory_order_acquire) && m_ctx->isCompiled &&
           m_ctx->isVerified;
}

bool CompilationSigningPage::validatePage() { return isComplete(); }

/* ========================================================================= */
/* Step 6: HubPublishingExportPage Implementation                            */
/* ========================================================================= */

HubPublishingExportPage::HubPublishingExportPage(CreatorPackagingContext *ctx, QWidget *parent)
    : QWizardPage(parent), m_ctx(ctx)
{
    setTitle(QStringLiteral("Step 6: Export & Install"));
    setSubTitle(QStringLiteral("Install the package locally or export it for sharing."));
    setupUi();
}

void HubPublishingExportPage::setupUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(12);

    // CID Banner
    auto *cidBox = new QGroupBox(QStringLiteral("Content Identifier (CID)"), this);
    auto *cidLayout = new QHBoxLayout(cidBox);
    cidLayout->setSpacing(8);

    m_cidLabel = new QLabel(QStringLiteral("sha256:..."), cidBox);
    m_cidLabel->setStyleSheet(QStringLiteral(
        "font-family: monospace; font-size: 12px; font-weight: bold; color: #38bdf8;"));
    m_cidLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    cidLayout->addWidget(m_cidLabel, 1);

    m_copyCidBtn = new QPushButton(QStringLiteral("Copy CID"), cidBox);
    cidLayout->addWidget(m_copyCidBtn);
    layout->addWidget(cidBox);

    // Registry Index Entry JSON
    auto *jsonBox = new QGroupBox(QStringLiteral("Registry Metadata"), this);
    auto *jsonLayout = new QVBoxLayout(jsonBox);
    jsonLayout->setSpacing(8);

    m_registryJsonEdit = new QTextEdit(jsonBox);
    m_registryJsonEdit->setReadOnly(true);
    m_registryJsonEdit->setStyleSheet(QStringLiteral(
        "background-color: #12141a; font-family: monospace; font-size: 11px; color: #e2e8f0;"));
    jsonLayout->addWidget(m_registryJsonEdit, 1);

    auto *btnRow = new QHBoxLayout();
    btnRow->setSpacing(8);
    m_copyJsonBtn = new QPushButton(QStringLiteral("Copy JSON"), jsonBox);
    btnRow->addWidget(m_copyJsonBtn);

    m_saveJsonBtn = new QPushButton(QStringLiteral("Save index.json..."), jsonBox);
    btnRow->addWidget(m_saveJsonBtn);
    btnRow->addStretch();
    jsonLayout->addLayout(btnRow);

    layout->addWidget(jsonBox, 1);

    // Actions Group
    auto *actGroup = new QGroupBox(QStringLiteral("Install & Export"), this);
    auto *actLayout = new QVBoxLayout(actGroup);
    actLayout->setSpacing(8);

    auto *installRow = new QHBoxLayout();
    installRow->setSpacing(8);
    m_installLocalBtn = new QPushButton(QStringLiteral("Install to Local Hub"), actGroup);
    m_installLocalBtn->setFixedHeight(32);
    m_installLocalBtn->setStyleSheet(
        QStringLiteral("background-color: #0284c7; color: white; font-weight: bold; font-size: "
                       "12px; padding: 6px 14px;"));
    installRow->addWidget(m_installLocalBtn);

    m_openFolderBtn = new QPushButton(QStringLiteral("Open Output Folder"), actGroup);
    m_openFolderBtn->setFixedHeight(32);
    installRow->addWidget(m_openFolderBtn);

    m_exportPkgBtn = new QPushButton(QStringLiteral("Save Package As..."), actGroup);
    m_exportPkgBtn->setFixedHeight(32);
    installRow->addWidget(m_exportPkgBtn);

    actLayout->addLayout(installRow);

    m_installStatusLabel =
        new QLabel(QStringLiteral("Ready to install to ~/.local/share/waywal/hub/."), actGroup);
    m_installStatusLabel->setStyleSheet(QStringLiteral("color: #94a3b8; font-size: 11px;"));
    actLayout->addWidget(m_installStatusLabel);

    layout->addWidget(actGroup);

    // Wiring
    connect(m_copyCidBtn, &QPushButton::clicked, this,
            &HubPublishingExportPage::copyCidToClipboard);
    connect(m_copyJsonBtn, &QPushButton::clicked, this,
            &HubPublishingExportPage::copyJsonToClipboard);
    connect(m_saveJsonBtn, &QPushButton::clicked, this, &HubPublishingExportPage::exportJsonToFile);
    connect(m_installLocalBtn, &QPushButton::clicked, this,
            &HubPublishingExportPage::installToLocalHub);
    connect(m_openFolderBtn, &QPushButton::clicked, this,
            &HubPublishingExportPage::openContainingFolder);
    connect(m_exportPkgBtn, &QPushButton::clicked, this, &HubPublishingExportPage::exportPackageTo);
}

void HubPublishingExportPage::initializePage()
{
    if (!m_ctx)
        return;

    // Compute CID from Merkle root
    m_ctx->cid = QStringLiteral("sha256:") + QString::fromLatin1(m_ctx->merkleRoot.toHex());
    m_cidLabel->setText(m_ctx->cid);

    // Construct Open Hub Registry Index Entry JSON
    QJsonObject root;
    root.insert(QStringLiteral("schema_version"), QStringLiteral("openhub/v1"));
    root.insert(QStringLiteral("cid"), m_ctx->cid);
    root.insert(QStringLiteral("id"), m_ctx->packageId);
    root.insert(QStringLiteral("slug"), m_ctx->slug);
    root.insert(QStringLiteral("name"), m_ctx->name);
    root.insert(QStringLiteral("version"), m_ctx->version);

    QJsonObject authorObj;
    authorObj.insert(QStringLiteral("name"), m_ctx->authorName);
    authorObj.insert(QStringLiteral("email"), m_ctx->authorEmail);
    authorObj.insert(QStringLiteral("pubkey"), QString::fromLatin1(m_ctx->authorPubKey.toHex()));
    root.insert(QStringLiteral("author"), authorObj);

    root.insert(QStringLiteral("license"), m_ctx->license);
    root.insert(QStringLiteral("description"), m_ctx->description);
    root.insert(QStringLiteral("type"), m_ctx->projectTypeStr);

    QJsonArray tagsArr;
    for (const auto &t : m_ctx->tags) {
        tagsArr.append(t);
    }
    root.insert(QStringLiteral("tags"), tagsArr);

    QJsonObject entrypointObj;
    entrypointObj.insert(QStringLiteral("pipeline"), m_ctx->entrypointPipeline);
    entrypointObj.insert(QStringLiteral("preview"), QStringLiteral("preview.webp"));
    entrypointObj.insert(QStringLiteral("preview_card"), QStringLiteral("preview_card.jpg"));
    if (!m_ctx->entrypointAudio.isEmpty()) {
        entrypointObj.insert(QStringLiteral("audio"), m_ctx->entrypointAudio);
    }
    root.insert(QStringLiteral("entrypoint"), entrypointObj);

    QJsonObject capObj;
    capObj.insert(QStringLiteral("compute_shaders"), m_ctx->capComputeShaders);
    capObj.insert(QStringLiteral("audio_spectrum"), m_ctx->capAudioSpectrum);
    capObj.insert(QStringLiteral("audio_playback"), m_ctx->capAudioPlayback);
    capObj.insert(QStringLiteral("scripting"), m_ctx->capScripting);
    capObj.insert(QStringLiteral("foreign_toplevel"), m_ctx->capForeignToplevel);
    capObj.insert(QStringLiteral("network"), false);
    capObj.insert(QStringLiteral("filesystem"), false);
    root.insert(QStringLiteral("capabilities"), capObj);

    QJsonObject budgetObj;
    budgetObj.insert(QStringLiteral("max_vram_mb"), static_cast<qint64>(m_ctx->maxVramMb));
    budgetObj.insert(QStringLiteral("target_fps"), static_cast<qint64>(m_ctx->targetFps));
    root.insert(QStringLiteral("resource_budgets"), budgetObj);

    root.insert(QStringLiteral("merkle_root"), QString::fromLatin1(m_ctx->merkleRoot.toHex()));
    root.insert(QStringLiteral("signature"), QString::fromLatin1(m_ctx->signature.toHex()));
    root.insert(QStringLiteral("file_size_bytes"), m_ctx->compiledSizeBytes);
    root.insert(QStringLiteral("published_at"), QDateTime::currentSecsSinceEpoch());

    QJsonDocument doc(root);
    m_ctx->registryIndexJson = QString::fromUtf8(doc.toJson(QJsonDocument::Indented));
    m_registryJsonEdit->setText(m_ctx->registryIndexJson);
}

void HubPublishingExportPage::copyCidToClipboard()
{
    if (m_ctx && !m_ctx->cid.isEmpty()) {
        QGuiApplication::clipboard()->setText(m_ctx->cid);
        m_copyCidBtn->setText(QStringLiteral("Copied"));
    }
}

void HubPublishingExportPage::copyJsonToClipboard()
{
    if (m_ctx && !m_ctx->registryIndexJson.isEmpty()) {
        QGuiApplication::clipboard()->setText(m_ctx->registryIndexJson);
        m_copyJsonBtn->setText(QStringLiteral("Copied"));
    }
}

void HubPublishingExportPage::exportJsonToFile()
{
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save Index JSON"),
                                                QDir::homePath() + QStringLiteral("/index.json"),
                                                QStringLiteral("JSON Files (*.json)"));
    if (!path.isEmpty() && m_ctx) {
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(m_ctx->registryIndexJson.toUtf8());
            f.close();
            QMessageBox::information(this, QStringLiteral("Saved"),
                                     QStringLiteral("Metadata saved to:\n%1").arg(path));
        }
    }
}

void HubPublishingExportPage::installToLocalHub()
{
    if (!m_ctx || m_ctx->outPkgPath.isEmpty() || !QFile::exists(m_ctx->outPkgPath)) {
        return;
    }

    const QString hubDir = QDir::homePath() + QStringLiteral("/.local/share/waywal/hub");
    QDir().mkpath(hubDir);

    QFileInfo outFi(m_ctx->outPkgPath);
    QString dstPkg = hubDir + QStringLiteral("/") + outFi.fileName();

    QFile::remove(dstPkg);
    if (!QFile::copy(m_ctx->outPkgPath, dstPkg)) {
        m_installStatusLabel->setText(
            QStringLiteral("Failed to copy package to local hub directory."));
        m_installStatusLabel->setStyleSheet(QStringLiteral("color: #ef4444;"));
        return;
    }

    // Update ~/.local/share/waywal/hub/catalog.json
    QString catPath = hubDir + QStringLiteral("/catalog.json");
    QJsonArray catArray;
    QFile readCat(catPath);
    if (readCat.open(QIODevice::ReadOnly)) {
        QJsonDocument d = QJsonDocument::fromJson(readCat.readAll());
        readCat.close();
        if (d.isArray()) {
            catArray = d.array();
        }
    }

    // Replace existing slug entry or append
    QJsonDocument itemDoc = QJsonDocument::fromJson(m_ctx->registryIndexJson.toUtf8());
    if (itemDoc.isObject()) {
        QJsonObject newObj = itemDoc.object();
        newObj.insert(QStringLiteral("local_package_path"), dstPkg);
        bool found = false;
        for (int i = 0; i < catArray.size(); i++) {
            if (catArray[i].toObject().value(QStringLiteral("slug")).toString() == m_ctx->slug) {
                catArray[i] = newObj;
                found = true;
                break;
            }
        }
        if (!found) {
            catArray.append(newObj);
        }

        QFile writeCat(catPath);
        if (writeCat.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            writeCat.write(QJsonDocument(catArray).toJson(QJsonDocument::Indented));
            writeCat.close();
        }
    }

    m_ctx->installedToLocalHub = true;
    m_installStatusLabel->setText(QStringLiteral("Installed to %1.").arg(dstPkg));
    m_installStatusLabel->setStyleSheet(QStringLiteral("color: #10b981; font-weight: bold;"));
    emit packageInstallRequested(dstPkg, QStringLiteral("*"));
}

void HubPublishingExportPage::openContainingFolder()
{
    if (m_ctx && !m_ctx->outPkgPath.isEmpty()) {
        QFileInfo fi(m_ctx->outPkgPath);
        QDesktopServices::openUrl(QUrl::fromLocalFile(fi.dir().absolutePath()));
    }
}

void HubPublishingExportPage::exportPackageTo()
{
    if (!m_ctx || m_ctx->outPkgPath.isEmpty())
        return;

    QString dst = QFileDialog::getSaveFileName(this, QStringLiteral("Save Package As"),
                                               QFileInfo(m_ctx->outPkgPath).fileName(),
                                               QStringLiteral("WayWal Package (*.wwpkg)"));
    if (!dst.isEmpty()) {
        QFile::remove(dst);
        if (QFile::copy(m_ctx->outPkgPath, dst)) {
            QMessageBox::information(this, QStringLiteral("Export Complete"),
                                     QStringLiteral("Package saved to:\n%1").arg(dst));
        }
    }
}

/* ========================================================================= */
/* CreatorPackagingWizard Top-Level Controller                               */
/* ========================================================================= */

CreatorPackagingWizard::CreatorPackagingWizard(QWidget *parent) : QWizard(parent)
{
    setObjectName(QStringLiteral("CreatorPackagingWizard"));
    setWindowTitle(QStringLiteral("Package Wallpaper"));
    setWizardStyle(QWizard::ModernStyle);
    resize(920, 680);

    // Instantiate all 6 guided pages
    m_intakePage = new SourceIntakePage(&m_context, this);
    m_bundlingPage = new AssetBundlingPage(&m_context, this);
    m_metadataPage = new MetadataPreviewPage(&m_context, this);
    m_auditPage = new CapabilityAuditPage(&m_context, this);
    m_compilationPage = new CompilationSigningPage(&m_context, this);
    m_exportPage = new HubPublishingExportPage(&m_context, this);

    setPage(Page_SourceIntake, m_intakePage);
    setPage(Page_AssetBundling, m_bundlingPage);
    setPage(Page_MetadataPreview, m_metadataPage);
    setPage(Page_CapabilityAudit, m_auditPage);
    setPage(Page_CompilationSigning, m_compilationPage);
    setPage(Page_HubPublishingExport, m_exportPage);

    connect(m_exportPage, &HubPublishingExportPage::packageInstallRequested, this,
            &CreatorPackagingWizard::packageInstallRequested);

    applyStudioTheme();
}

CreatorPackagingWizard::~CreatorPackagingWizard()
{
    wipeByteArray(m_context.authorPrivKey);

    // Clean up temporary staging directory if located in QDir::tempPath
    if (!m_context.stagingDir.isEmpty() && m_context.stagingDir.startsWith(QDir::tempPath())) {
        QDir stg(m_context.stagingDir);
        stg.removeRecursively();
    }
}

void CreatorPackagingWizard::applyStudioTheme()
{
    // Clean dark aesthetic matching wwal-studio
    setStyleSheet(
        QStringLiteral("QWizard {"
                       "  background-color: #181b22;"
                       "  color: #f1f5f9;"
                       "}"
                       "QWizardPage {"
                       "  background-color: #181b22;"
                       "  color: #f1f5f9;"
                       "}"
                       "QGroupBox {"
                       "  border: 1px solid #2c3240;"
                       "  border-radius: 6px;"
                       "  margin-top: 10px;"
                       "  padding-top: 10px;"
                       "  font-weight: bold;"
                       "  color: #cbd5e1;"
                       "}"
                       "QGroupBox::title {"
                       "  subcontrol-origin: margin;"
                       "  subcontrol-position: top left;"
                       "  left: 10px;"
                       "  padding: 0 4px;"
                       "}"
                       "QLineEdit, QComboBox, QSpinBox, QTextEdit {"
                       "  background-color: #12141a;"
                       "  border: 1px solid #2c3240;"
                       "  border-radius: 5px;"
                       "  padding: 5px 8px;"
                       "  color: #f1f5f9;"
                       "}"
                       "QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QTextEdit:focus {"
                       "  border: 1px solid #38bdf8;"
                       "}"
                       "QPushButton {"
                       "  background-color: #20242e;"
                       "  border: 1px solid #2c3240;"
                       "  border-radius: 5px;"
                       "  padding: 6px 14px;"
                       "  color: #f1f5f9;"
                       "}"
                       "QPushButton:hover {"
                       "  background-color: #2a303d;"
                       "}"
                       "QPushButton:pressed {"
                       "  background-color: #12141a;"
                       "}"
                       "QTreeWidget {"
                       "  background-color: #12141a;"
                       "  border: 1px solid #2c3240;"
                       "  border-radius: 5px;"
                       "  color: #f1f5f9;"
                       "}"
                       "QHeaderView::section {"
                       "  background-color: #20242e;"
                       "  color: #94a3b8;"
                       "  padding: 5px;"
                       "  border: 1px solid #2c3240;"
                       "}"
                       "QProgressBar {"
                       "  background-color: #12141a;"
                       "  border: 1px solid #2c3240;"
                       "  border-radius: 4px;"
                       "  text-align: center;"
                       "  color: white;"
                       "}"
                       "QProgressBar::chunk {"
                       "  background-color: #0284c7;"
                       "}"));
}

void CreatorPackagingWizard::setSourcePath(const QString &path)
{
    m_context.sourcePath = path;
    if (m_intakePage) {
        m_intakePage->setSourcePath(path);
    }
}

QString CreatorPackagingWizard::sourcePath() const { return m_context.sourcePath; }

QString CreatorPackagingWizard::compiledPackagePath() const { return m_context.outPkgPath; }

QString CreatorPackagingWizard::contentIdentifier() const { return m_context.cid; }

} // namespace wwal::studio
