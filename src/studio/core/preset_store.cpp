// SPDX-License-Identifier: GPL-3.0-or-later
#include "preset_store.hpp"

#include "waywal/log.h"
#include "waywal/path.h"

#include <QColor>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QRegularExpression>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace waywal::studio {

namespace {

constexpr size_t kMaxPresetFileBytes = 16U * 1024U * 1024U;
constexpr qsizetype kMaxPresetIdChars = 200;

[[nodiscard]] bool writeAllFd(int fd, const char *data, size_t size) noexcept
{
    size_t written = 0;
    while (written < size) {
        const ssize_t rc = ::write(fd, data + written, size - written);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (rc == 0) {
            errno = EIO;
            return false;
        }
        written += static_cast<size_t>(rc);
    }
    return true;
}

[[nodiscard]] bool fsyncFd(int fd) noexcept
{
    while (::fsync(fd) != 0) {
        if (errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

[[nodiscard]] bool fsyncDirFd(int dirFd) noexcept
{
    while (::fsync(dirFd) != 0) {
        if (errno == EINTR) {
            continue;
        }
        if (errno == EINVAL || errno == ENOTSUP) {
            // Virtual filesystems such as tmpfs/FUSE variants may reject directory fsync.
            return true;
        }
        return false;
    }
    return true;
}

[[nodiscard]] bool closeFd(int fd) noexcept { return ::close(fd) == 0 || errno == EINTR; }

[[nodiscard]] bool openPresetDirFd(const QString &dirPath, bool createIfMissing, int &outDirFd,
                                   QString *errorOut)
{
    outDirFd = -1;
    if (dirPath.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Preset directory path is empty");
        }
        return false;
    }

    QDir dir(dirPath);
    if (!dir.exists()) {
        if (!createIfMissing) {
            if (errorOut) {
                *errorOut = QStringLiteral("Preset directory does not exist: ") + dirPath;
            }
            return false;
        }
        if (!dir.mkpath(QStringLiteral("."))) {
            if (errorOut) {
                *errorOut = QStringLiteral("Failed to create preset directory: ") + dirPath;
            }
            return false;
        }
    }

    const QByteArray dirNative = QFile::encodeName(dirPath);
    int dirFd = -1;
    do {
        // O_NOFOLLOW rejects a symlinked leaf directory while permitting symlinked parent dirs.
        dirFd = ::open(dirNative.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    } while (dirFd < 0 && errno == EINTR);

    if (dirFd < 0) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to open preset directory ") + dirPath +
                        QStringLiteral(" (") + QString::fromLocal8Bit(std::strerror(errno)) +
                        QStringLiteral(")");
        }
        return false;
    }

    outDirFd = dirFd;
    return true;
}

[[nodiscard]] bool validateConfinedPresetFileName(const QString &fileName, QString *errorOut)
{
    if (fileName.isEmpty() || fileName == QLatin1Char('.') || fileName == QLatin1String("..") ||
        fileName.startsWith(QLatin1Char('.')) || fileName.contains(QLatin1Char('/')) ||
        fileName.contains(QLatin1Char('\\')) || fileName.contains(QLatin1Char('\0'))) {
        if (errorOut) {
            *errorOut = QStringLiteral("Unsafe preset file name rejected: ") + fileName;
        }
        return false;
    }
    return true;
}

[[nodiscard]] bool atomicWriteFileInDir(int dirFd, const QByteArray &fileNameNative,
                                        const QByteArray &bytes, QString *errorOut)
{
    struct stat existingSt{};
    if (::fstatat(dirFd, fileNameNative.constData(), &existingSt, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(existingSt.st_mode)) {
            if (errorOut) {
                *errorOut =
                    QStringLiteral("Refusing to overwrite non-regular or symlink preset: ") +
                    QFile::decodeName(fileNameNative);
            }
            return false;
        }
    } else if (errno != ENOENT) {
        if (errorOut) {
            *errorOut = QStringLiteral("Cannot inspect preset target ") +
                        QFile::decodeName(fileNameNative) + QStringLiteral(" (") +
                        QString::fromLocal8Bit(std::strerror(errno)) + QStringLiteral(")");
        }
        return false;
    }

    static std::atomic<quint64> s_seq{0};
    const qint64 pid = QCoreApplication::applicationPid();

    QByteArray tmpName;
    int fd = -1;
    for (int attempt = 0; attempt < 16; ++attempt) {
        const quint64 seq = s_seq.fetch_add(1, std::memory_order_relaxed);
        const quint32 rnd = QRandomGenerator::global()->generate();
        tmpName = QByteArray(".wwal_preset.tmp.") + QByteArray::number(pid) + '.' +
                  QByteArray::number(seq) + '.' + QByteArray::number(rnd);

        fd = ::openat(dirFd, tmpName.constData(),
                      O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (fd >= 0) {
            break;
        }
        if (errno != EEXIST) {
            if (errorOut) {
                *errorOut = QStringLiteral("Failed to create temporary preset file ") +
                            QFile::decodeName(tmpName) + QStringLiteral(" (") +
                            QString::fromLocal8Bit(std::strerror(errno)) + QStringLiteral(")");
            }
            return false;
        }
    }

    if (fd < 0) {
        if (errorOut) {
            *errorOut = QStringLiteral("Exhausted unique temporary file names for preset");
        }
        return false;
    }

    bool ok = writeAllFd(fd, bytes.constData(), static_cast<size_t>(bytes.size()));
    int savedErr = ok ? 0 : errno;
    if (ok && !fsyncFd(fd)) {
        ok = false;
        savedErr = errno;
    }
    if (!closeFd(fd) && ok) {
        ok = false;
        savedErr = errno;
    }

    if (!ok) {
        ::unlinkat(dirFd, tmpName.constData(), 0);
        if (errorOut) {
            *errorOut = QStringLiteral("Failed writing preset file ") +
                        QFile::decodeName(fileNameNative) + QStringLiteral(" (") +
                        QString::fromLocal8Bit(std::strerror(savedErr)) + QStringLiteral(")");
        }
        return false;
    }

    // Re-check target entry right before renameat to shrink the symlink swap window.
    if (::fstatat(dirFd, fileNameNative.constData(), &existingSt, AT_SYMLINK_NOFOLLOW) == 0 &&
        !S_ISREG(existingSt.st_mode)) {
        ::unlinkat(dirFd, tmpName.constData(), 0);
        if (errorOut) {
            *errorOut = QStringLiteral("Refusing to replace symlink or non-regular target: ") +
                        QFile::decodeName(fileNameNative);
        }
        return false;
    }

    if (::renameat(dirFd, tmpName.constData(), dirFd, fileNameNative.constData()) != 0) {
        savedErr = errno;
        ::unlinkat(dirFd, tmpName.constData(), 0);
        if (errorOut) {
            *errorOut = QStringLiteral("Atomic rename failed for preset ") +
                        QFile::decodeName(fileNameNative) + QStringLiteral(" (") +
                        QString::fromLocal8Bit(std::strerror(savedErr)) + QStringLiteral(")");
        }
        return false;
    }

    if (!fsyncDirFd(dirFd)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to sync preset parent directory (") +
                        QString::fromLocal8Bit(std::strerror(errno)) + QStringLiteral(")");
        }
        return false;
    }

    return true;
}

[[nodiscard]] bool readRegularFileInDir(int dirFd, const QByteArray &fileNameNative,
                                        QByteArray &outBytes, QString *errorOut)
{
    int fd = -1;
    do {
        fd = ::openat(dirFd, fileNameNative.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);

    if (fd < 0) {
        if (errorOut) {
            *errorOut = QStringLiteral("Preset not found or unsafe: ") +
                        QFile::decodeName(fileNameNative) + QStringLiteral(" (") +
                        QString::fromLocal8Bit(std::strerror(errno)) + QStringLiteral(")");
        }
        return false;
    }

    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        const int err = errno;
        (void)closeFd(fd);
        if (errorOut) {
            *errorOut = QStringLiteral("Preset target is not a regular file: ") +
                        QFile::decodeName(fileNameNative) + QStringLiteral(" (") +
                        QString::fromLocal8Bit(std::strerror(err)) + QStringLiteral(")");
        }
        return false;
    }

    if (st.st_size < 0 || static_cast<size_t>(st.st_size) > kMaxPresetFileBytes) {
        (void)closeFd(fd);
        if (errorOut) {
            *errorOut = QStringLiteral("Preset file exceeds size limit: ") +
                        QFile::decodeName(fileNameNative);
        }
        return false;
    }

    outBytes.clear();
    if (st.st_size > 0) {
        outBytes.reserve(static_cast<qsizetype>(st.st_size));
    }

    char buf[8192];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int err = errno;
            (void)closeFd(fd);
            if (errorOut) {
                *errorOut = QStringLiteral("Failed reading preset file: ") +
                            QFile::decodeName(fileNameNative) + QStringLiteral(" (") +
                            QString::fromLocal8Bit(std::strerror(err)) + QStringLiteral(")");
            }
            return false;
        }
        if (n == 0) {
            break;
        }
        if (static_cast<size_t>(outBytes.size()) + static_cast<size_t>(n) > kMaxPresetFileBytes) {
            (void)closeFd(fd);
            if (errorOut) {
                *errorOut = QStringLiteral("Preset file exceeded size limit during read: ") +
                            QFile::decodeName(fileNameNative);
            }
            return false;
        }
        outBytes.append(buf, static_cast<qsizetype>(n));
    }

    (void)closeFd(fd);
    return true;
}

[[nodiscard]] bool isTomlBareKeyChar(QChar ch) noexcept
{
    const ushort u = ch.unicode();
    return (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '_' ||
           u == '-';
}

[[nodiscard]] bool isTomlBareKey(QStringView sv) noexcept
{
    if (sv.isEmpty()) {
        return false;
    }
    for (QChar ch : sv) {
        if (!isTomlBareKeyChar(ch)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] QString escapeTomlBasicString(QStringView sv)
{
    QString out;
    out.reserve(sv.size() + 8);
    for (QChar ch : sv) {
        const ushort uc = ch.unicode();
        switch (uc) {
        case '\\':
            out += QStringLiteral("\\\\");
            break;
        case '"':
            out += QStringLiteral("\\\"");
            break;
        case '\b':
            out += QStringLiteral("\\b");
            break;
        case '\t':
            out += QStringLiteral("\\t");
            break;
        case '\n':
            out += QStringLiteral("\\n");
            break;
        case '\f':
            out += QStringLiteral("\\f");
            break;
        case '\r':
            out += QStringLiteral("\\r");
            break;
        default:
            if (uc < 0x20 || uc == 0x7F) {
                out += QStringLiteral("\\u") +
                       QString::number(uc, 16).rightJustified(4, QLatin1Char('0')).toUpper();
            } else {
                out += ch;
            }
            break;
        }
    }
    return out;
}

[[nodiscard]] QString unescapeTomlBasicString(QStringView sv, bool *okOut = nullptr)
{
    if (okOut) {
        *okOut = true;
    }

    auto parseHexCodepoint = [](QStringView hex, uint32_t &outCp) -> bool {
        uint32_t cp = 0;
        for (QChar hc : hex) {
            const ushort u = hc.unicode();
            uint32_t nib = 0;
            if (u >= '0' && u <= '9') {
                nib = u - '0';
            } else if (u >= 'a' && u <= 'f') {
                nib = 10u + (u - 'a');
            } else if (u >= 'A' && u <= 'F') {
                nib = 10u + (u - 'A');
            } else {
                return false;
            }
            cp = (cp << 4) | nib;
        }
        // TOML 1.0 forbids UTF-16 surrogates and values above U+10FFFF.
        if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
            return false;
        }
        outCp = cp;
        return true;
    };

    QString out;
    out.reserve(sv.size());
    for (qsizetype i = 0; i < sv.size(); ++i) {
        const QChar ch = sv[i];
        if (ch != QLatin1Char('\\')) {
            out += ch;
            continue;
        }
        if (i + 1 >= sv.size()) {
            if (okOut) {
                *okOut = false;
            }
            return {};
        }
        const QChar next = sv[++i];
        switch (next.unicode()) {
        case '\\':
            out += QLatin1Char('\\');
            break;
        case '"':
            out += QLatin1Char('"');
            break;
        case 'b':
            out += QLatin1Char('\b');
            break;
        case 't':
            out += QLatin1Char('\t');
            break;
        case 'n':
            out += QLatin1Char('\n');
            break;
        case 'f':
            out += QLatin1Char('\f');
            break;
        case 'r':
            out += QLatin1Char('\r');
            break;
        case 'u': {
            if (i + 4 >= sv.size()) {
                if (okOut) {
                    *okOut = false;
                }
                return {};
            }
            uint32_t cp = 0;
            if (!parseHexCodepoint(sv.mid(i + 1, 4), cp)) {
                if (okOut) {
                    *okOut = false;
                }
                return {};
            }
            out += QChar(static_cast<char16_t>(cp));
            i += 4;
            break;
        }
        case 'U': {
            if (i + 8 >= sv.size()) {
                if (okOut) {
                    *okOut = false;
                }
                return {};
            }
            uint32_t cp = 0;
            if (!parseHexCodepoint(sv.mid(i + 1, 8), cp)) {
                if (okOut) {
                    *okOut = false;
                }
                return {};
            }
            const char32_t ucs4 = static_cast<char32_t>(cp);
            out += QString::fromUcs4(&ucs4, 1);
            i += 8;
            break;
        }
        default:
            if (okOut) {
                *okOut = false;
            }
            return {};
        }
    }
    return out;
}

[[nodiscard]] QString formatTomlKey(QStringView key)
{
    if (isTomlBareKey(key)) {
        return key.toString();
    }
    return QLatin1Char('"') + escapeTomlBasicString(key) + QLatin1Char('"');
}

[[nodiscard]] QString formatTomlFloat(float val, int decimals)
{
    const float normalized = (val == 0.0f) ? 0.0f : val;
    return QString::number(normalized, 'f', decimals);
}

[[nodiscard]] bool parseFiniteFloat(QStringView raw, float &out)
{
    const QString trimmed = raw.trimmed().toString();
    if (trimmed.isEmpty()) {
        return false;
    }

    bool okDouble = false;
    const double dVal = trimmed.toDouble(&okDouble);
    if (!okDouble || !std::isfinite(dVal)) {
        return false;
    }

    bool okFloat = false;
    const float fVal = trimmed.toFloat(&okFloat);
    if (okFloat && std::isfinite(fVal)) {
        out = fVal;
        return true;
    }

    // Handle subnormal double values that underflow float while staying within float bounds.
    if (std::fabs(dVal) <= static_cast<double>(std::numeric_limits<float>::max())) {
        out = static_cast<float>(dVal);
        return std::isfinite(out);
    }
    return false;
}

[[nodiscard]] bool parseUInt32(QStringView raw, uint32_t &out)
{
    bool ok = false;
    const qulonglong val = raw.trimmed().toString().toULongLong(&ok, 10);
    if (!ok || val > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    out = static_cast<uint32_t>(val);
    return true;
}

[[nodiscard]] bool parseNumberArray(QStringView valStr, std::vector<float> &outNums)
{
    outNums.clear();
    QStringView trimmed = valStr.trimmed();
    if (!trimmed.startsWith(QLatin1Char('[')) || !trimmed.endsWith(QLatin1Char(']')) ||
        trimmed.size() < 2) {
        return false;
    }

    const QString inner = trimmed.mid(1, trimmed.size() - 2).toString();
    const QStringList parts = inner.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        return false;
    }

    outNums.reserve(static_cast<size_t>(parts.size()));
    for (const QString &part : parts) {
        float v = 0.0f;
        if (!parseFiniteFloat(part, v)) {
            outNums.clear();
            return false;
        }
        outNums.push_back(v);
    }
    return true;
}

[[nodiscard]] QString stripTomlInlineComment(QStringView raw)
{
    bool inBasic = false;
    bool inLiteral = false;
    bool escaped = false;
    int bracketDepth = 0;

    for (qsizetype i = 0; i < raw.size(); ++i) {
        const QChar ch = raw[i];
        if (inBasic) {
            if (escaped) {
                escaped = false;
            } else if (ch == QLatin1Char('\\')) {
                escaped = true;
            } else if (ch == QLatin1Char('"')) {
                inBasic = false;
            }
            continue;
        }
        if (inLiteral) {
            if (ch == QLatin1Char('\'')) {
                inLiteral = false;
            }
            continue;
        }
        if (ch == QLatin1Char('"')) {
            inBasic = true;
        } else if (ch == QLatin1Char('\'')) {
            inLiteral = true;
        } else if (ch == QLatin1Char('[')) {
            ++bracketDepth;
        } else if (ch == QLatin1Char(']') && bracketDepth > 0) {
            --bracketDepth;
        } else if (ch == QLatin1Char('#') && bracketDepth == 0) {
            return raw.left(i).trimmed().toString();
        }
    }

    return raw.trimmed().toString();
}

[[nodiscard]] bool parseTomlKeyValueLine(QStringView line, QString &outKey, QString &outRawVal,
                                         QString &outUnquoted, bool &outIsString,
                                         bool &outStringValid)
{
    qsizetype pos = 0;
    const qsizetype n = line.size();
    while (pos < n && line[pos].isSpace()) {
        ++pos;
    }
    if (pos >= n) {
        return false;
    }

    outKey.clear();
    if (line[pos] == QLatin1Char('"')) {
        ++pos;
        const qsizetype start = pos;
        bool escaped = false;
        bool foundEnd = false;
        while (pos < n) {
            const QChar ch = line[pos];
            if (escaped) {
                escaped = false;
            } else if (ch == QLatin1Char('\\')) {
                escaped = true;
            } else if (ch == QLatin1Char('"')) {
                foundEnd = true;
                break;
            }
            ++pos;
        }
        if (!foundEnd) {
            return false;
        }
        bool unescapeOk = false;
        outKey = unescapeTomlBasicString(line.mid(start, pos - start), &unescapeOk);
        if (!unescapeOk) {
            return false;
        }
        ++pos;
    } else if (line[pos] == QLatin1Char('\'')) {
        ++pos;
        const qsizetype start = pos;
        while (pos < n && line[pos] != QLatin1Char('\'')) {
            ++pos;
        }
        if (pos >= n) {
            return false;
        }
        outKey = line.mid(start, pos - start).toString();
        ++pos;
    } else {
        const qsizetype start = pos;
        while (pos < n && !line[pos].isSpace() && line[pos] != QLatin1Char('=')) {
            ++pos;
        }
        if (pos == start) {
            return false;
        }
        outKey = line.mid(start, pos - start).toString();
    }

    while (pos < n && line[pos].isSpace()) {
        ++pos;
    }
    if (pos >= n || line[pos] != QLatin1Char('=')) {
        return false;
    }
    ++pos;

    outRawVal = stripTomlInlineComment(line.mid(pos));
    if (outRawVal.isEmpty()) {
        return false;
    }

    outIsString = false;
    outStringValid = true;
    outUnquoted = outRawVal;

    if (outRawVal.startsWith(QLatin1Char('"'))) {
        outIsString = true;
        if (outRawVal.size() < 2 || !outRawVal.endsWith(QLatin1Char('"'))) {
            outStringValid = false;
            return true;
        }
        bool unescapeOk = false;
        outUnquoted = unescapeTomlBasicString(QStringView(outRawVal).mid(1, outRawVal.size() - 2),
                                              &unescapeOk);
        outStringValid = unescapeOk;
    } else if (outRawVal.startsWith(QLatin1Char('\''))) {
        outIsString = true;
        if (outRawVal.size() < 2 || !outRawVal.endsWith(QLatin1Char('\''))) {
            outStringValid = false;
            return true;
        }
        outUnquoted = outRawVal.mid(1, outRawVal.size() - 2);
    }

    return true;
}

[[nodiscard]] bool validateFinitePreset(const WallpaperPreset &preset, QString *errorOut)
{
    if (!std::isfinite(preset.bezier.p1x) || !std::isfinite(preset.bezier.p1y) ||
        !std::isfinite(preset.bezier.p2x) || !std::isfinite(preset.bezier.p2y) ||
        !std::isfinite(preset.volume) || !std::isfinite(preset.cropGravityX) ||
        !std::isfinite(preset.cropGravityY)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Preset contains non-finite numeric parameter");
        }
        return false;
    }

    for (const auto &[name, uval] : preset.uniforms) {
        if (!uval.isFinite()) {
            if (errorOut) {
                *errorOut = QStringLiteral("Preset uniform contains non-finite value: ") + name;
            }
            return false;
        }
    }
    return true;
}

} // namespace

/* ========================================================================= */
/* PresetStore Implementation                                                */
/* ========================================================================= */

PresetStore::PresetStore(QString baseDir, QObject *parent)
    : QObject(parent), m_baseDir(std::move(baseDir))
{
    ensureDirectoryExists();
}

QString PresetStore::sanitizeId(const QString &id)
{
    if (id.trimmed().isEmpty()) {
        return QStringLiteral("default");
    }

    QString sanitized = id;
    static const QRegularExpression unsafeChars(QStringLiteral("[^a-zA-Z0-9_.-]"));
    sanitized.replace(unsafeChars, QStringLiteral("_"));

    // Collapse multi-dot runs so traversal segments like ".." never survive.
    static const QRegularExpression multiDots(QStringLiteral("\\.{2,}"));
    sanitized.replace(multiDots, QStringLiteral("_"));

    if (sanitized.size() > kMaxPresetIdChars) {
        sanitized.truncate(kMaxPresetIdChars);
    }

    // Leading or trailing dots would produce hidden dotfiles (".toml", "..toml").
    if (sanitized.startsWith(QLatin1Char('.'))) {
        sanitized[0] = QLatin1Char('_');
    }
    if (sanitized.endsWith(QLatin1Char('.'))) {
        sanitized[sanitized.size() - 1] = QLatin1Char('_');
    }

    const bool hasAlnum = std::any_of(sanitized.cbegin(), sanitized.cend(), [](QChar ch) {
        const ushort u = ch.unicode();
        return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9');
    });
    if (!hasAlnum) {
        return QStringLiteral("default");
    }

    return sanitized;
}

QString PresetStore::presetDirectory() const
{
    if (!m_baseDir.isEmpty()) {
        return QDir::cleanPath(m_baseDir);
    }

    QString xdgConfig = qEnvironmentVariable("XDG_CONFIG_HOME");
    // XDG Base Directory spec requires ignoring unset or relative XDG_CONFIG_HOME values.
    if (!xdgConfig.isEmpty() && QDir::isAbsolutePath(xdgConfig)) {
        return QDir::cleanPath(xdgConfig + QStringLiteral("/wwal/presets"));
    }

    QString home = qEnvironmentVariable("HOME");
    if (home.isEmpty() || !QDir::isAbsolutePath(home)) {
        home = QDir::homePath();
    }
    if (!home.isEmpty() && QDir::isAbsolutePath(home)) {
        return QDir::cleanPath(home + QStringLiteral("/.config/wwal/presets"));
    }

    return QStringLiteral("/tmp/wwal/presets");
}

QString PresetStore::resolvePresetFilePath(const QString &id) const
{
    const QString safeId = sanitizeId(id);
    return QDir(presetDirectory()).filePath(safeId + QStringLiteral(".toml"));
}

void PresetStore::ensureDirectoryExists() const
{
    const QString dirPath = presetDirectory();
    QDir dir(dirPath);
    if (!dir.exists()) {
        dir.mkpath(QStringLiteral("."));
    }
}

bool PresetStore::hasPreset(const QString &id) const
{
    const QString fileName = sanitizeId(id) + QStringLiteral(".toml");
    if (!validateConfinedPresetFileName(fileName, nullptr)) {
        return false;
    }

    int dirFd = -1;
    if (!openPresetDirFd(presetDirectory(), false, dirFd, nullptr)) {
        return false;
    }

    const QByteArray fileNameNative = QFile::encodeName(fileName);
    struct stat st{};
    const bool exists =
        (::fstatat(dirFd, fileNameNative.constData(), &st, AT_SYMLINK_NOFOLLOW) == 0 &&
         S_ISREG(st.st_mode));
    (void)closeFd(dirFd);
    return exists;
}

QStringList PresetStore::listPresetIds() const
{
    QDir dir(presetDirectory());
    if (!dir.exists()) {
        return {};
    }

    const QStringList filters{QStringLiteral("*.toml")};
    const QFileInfoList fileList =
        dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks, QDir::Name);

    QStringList ids;
    ids.reserve(fileList.size());
    for (const auto &info : fileList) {
        if (info.isSymLink() || info.fileName().startsWith(QLatin1Char('.'))) {
            continue;
        }
        const QString base = info.completeBaseName();
        if (!base.isEmpty() && base != QLatin1Char('.') && base != QLatin1String("..")) {
            ids.append(base);
        }
    }
    return ids;
}

QList<WallpaperPreset> PresetStore::listAllPresets() const
{
    const QStringList ids = listPresetIds();
    QList<WallpaperPreset> presets;
    presets.reserve(ids.size());

    for (const auto &id : ids) {
        WallpaperPreset p{};
        if (loadPresetInternal(id, p, nullptr)) {
            presets.append(std::move(p));
        }
    }
    return presets;
}

bool PresetStore::deletePreset(const QString &id)
{
    const QString fileName = sanitizeId(id) + QStringLiteral(".toml");
    QString err;
    if (!validateConfinedPresetFileName(fileName, &err)) {
        emit errorOccurred(err);
        return false;
    }

    int dirFd = -1;
    if (!openPresetDirFd(presetDirectory(), false, dirFd, nullptr)) {
        return false;
    }

    const QByteArray fileNameNative = QFile::encodeName(fileName);
    struct stat st{};
    if (::fstatat(dirFd, fileNameNative.constData(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        (void)closeFd(dirFd);
        return false;
    }

    if (!S_ISREG(st.st_mode)) {
        (void)closeFd(dirFd);
        emit errorOccurred(QStringLiteral("Refusing to delete non-regular or symlink preset: ") +
                           fileName);
        return false;
    }

    if (::unlinkat(dirFd, fileNameNative.constData(), 0) == 0) {
        (void)fsyncDirFd(dirFd);
        (void)closeFd(dirFd);
        emit presetDeleted(id);
        return true;
    }

    const int savedErr = errno;
    (void)closeFd(dirFd);
    emit errorOccurred(QStringLiteral("Failed to delete preset ") + fileName +
                       QStringLiteral(" (") + QString::fromLocal8Bit(std::strerror(savedErr)) +
                       QStringLiteral(")"));
    return false;
}

bool PresetStore::savePreset(const WallpaperPreset &preset)
{
    QString err;
    if (!validateFinitePreset(preset, &err)) {
        emit errorOccurred(err);
        return false;
    }

    const QString safeId = sanitizeId(preset.id);
    const QString fileName = safeId + QStringLiteral(".toml");
    if (!validateConfinedPresetFileName(fileName, &err)) {
        emit errorOccurred(err);
        return false;
    }

    QString tomlText;
    tomlText.reserve(1024);
    tomlText += QStringLiteral("# WayWal Wallpaper Preset\n");
    tomlText += QStringLiteral("# Automatically generated by wwal-studio\n\n");

    tomlText += QStringLiteral("[preset]\n");
    tomlText += QStringLiteral("id = \"") + escapeTomlBasicString(safeId) + QStringLiteral("\"\n");
    tomlText +=
        QStringLiteral("title = \"") + escapeTomlBasicString(preset.title) + QStringLiteral("\"\n");
    tomlText += QStringLiteral("wallpaper_path = \"") +
                escapeTomlBasicString(preset.wallpaperPath) + QStringLiteral("\"\n\n");

    tomlText += QStringLiteral("[transition]\n");
    tomlText += QStringLiteral("type = \"") + escapeTomlBasicString(preset.transitionType) +
                QStringLiteral("\"\n");
    tomlText += QStringLiteral("duration_ms = ") + QString::number(preset.transitionDurationMs) +
                QLatin1Char('\n');
    tomlText +=
        QStringLiteral("fps = ") + QString::number(preset.transitionFps) + QLatin1Char('\n');
    tomlText += QStringLiteral("has_custom_bezier = ") +
                (preset.hasCustomBezier ? QStringLiteral("true\n") : QStringLiteral("false\n"));
    tomlText += QStringLiteral("bezier = [") + formatTomlFloat(preset.bezier.p1x, 4) +
                QStringLiteral(", ") + formatTomlFloat(preset.bezier.p1y, 4) +
                QStringLiteral(", ") + formatTomlFloat(preset.bezier.p2x, 4) +
                QStringLiteral(", ") + formatTomlFloat(preset.bezier.p2y, 4) +
                QStringLiteral("]\n\n");

    tomlText += QStringLiteral("[power]\n");
    tomlText += QStringLiteral("battery_policy = \"") +
                escapeTomlBasicString(preset.batteryPolicy) + QStringLiteral("\"\n");
    tomlText += QStringLiteral("mute_on_audio = ") +
                (preset.muteOnAudio ? QStringLiteral("true\n") : QStringLiteral("false\n"));
    tomlText +=
        QStringLiteral("volume = ") + formatTomlFloat(preset.volume, 3) + QStringLiteral("\n\n");

    tomlText += QStringLiteral("[layout]\n");
    tomlText += QStringLiteral("crop_gravity_x = ") + formatTomlFloat(preset.cropGravityX, 4) +
                QLatin1Char('\n');
    tomlText += QStringLiteral("crop_gravity_y = ") + formatTomlFloat(preset.cropGravityY, 4) +
                QLatin1Char('\n');
    tomlText += QStringLiteral("has_crop_gravity = ") +
                (preset.hasCropGravity ? QStringLiteral("true\n\n") : QStringLiteral("false\n\n"));

    std::vector<QString> colorUniformKeys;
    if (!preset.uniforms.empty()) {
        tomlText += QStringLiteral("[uniforms]\n");
        for (const auto &[name, uval] : preset.uniforms) {
            const QString keyOut = formatTomlKey(name);
            switch (uval.type) {
            case UniformValue::Type::Float:
                tomlText += keyOut + QStringLiteral(" = ") + formatTomlFloat(uval.floats[0], 6) +
                            QLatin1Char('\n');
                break;
            case UniformValue::Type::Int:
                tomlText += keyOut + QStringLiteral(" = ") + QString::number(uval.intVal) +
                            QLatin1Char('\n');
                break;
            case UniformValue::Type::Bool:
                tomlText += keyOut + QStringLiteral(" = ") +
                            (uval.boolVal ? QStringLiteral("true\n") : QStringLiteral("false\n"));
                break;
            case UniformValue::Type::Vec2:
                tomlText += keyOut + QStringLiteral(" = [") + formatTomlFloat(uval.floats[0], 6) +
                            QStringLiteral(", ") + formatTomlFloat(uval.floats[1], 6) +
                            QStringLiteral("]\n");
                break;
            case UniformValue::Type::Vec3:
                tomlText += keyOut + QStringLiteral(" = [") + formatTomlFloat(uval.floats[0], 6) +
                            QStringLiteral(", ") + formatTomlFloat(uval.floats[1], 6) +
                            QStringLiteral(", ") + formatTomlFloat(uval.floats[2], 6) +
                            QStringLiteral("]\n");
                break;
            case UniformValue::Type::Vec4:
                tomlText += keyOut + QStringLiteral(" = [") + formatTomlFloat(uval.floats[0], 6) +
                            QStringLiteral(", ") + formatTomlFloat(uval.floats[1], 6) +
                            QStringLiteral(", ") + formatTomlFloat(uval.floats[2], 6) +
                            QStringLiteral(", ") + formatTomlFloat(uval.floats[3], 6) +
                            QStringLiteral("]\n");
                break;
            case UniformValue::Type::Color:
                colorUniformKeys.push_back(name);
                tomlText += keyOut + QStringLiteral(" = [") + formatTomlFloat(uval.floats[0], 6) +
                            QStringLiteral(", ") + formatTomlFloat(uval.floats[1], 6) +
                            QStringLiteral(", ") + formatTomlFloat(uval.floats[2], 6) +
                            QStringLiteral(", ") + formatTomlFloat(uval.floats[3], 6) +
                            QStringLiteral("]\n");
                break;
            case UniformValue::Type::String:
                tomlText += keyOut + QStringLiteral(" = \"") +
                            escapeTomlBasicString(uval.stringVal) + QStringLiteral("\"\n");
                break;
            }
        }
        tomlText += QLatin1Char('\n');
    }

    if (!colorUniformKeys.empty()) {
        tomlText += QStringLiteral("[uniform_types]\n");
        for (const QString &colorKey : colorUniformKeys) {
            tomlText += formatTomlKey(colorKey) + QStringLiteral(" = \"color\"\n");
        }
        tomlText += QLatin1Char('\n');
    }

    if (!preset.extraProperties.empty()) {
        tomlText += QStringLiteral("[extra]\n");
        for (const auto &[key, val] : preset.extraProperties) {
            tomlText += formatTomlKey(key) + QStringLiteral(" = \"") + escapeTomlBasicString(val) +
                        QStringLiteral("\"\n");
        }
        tomlText += QLatin1Char('\n');
    }

    int dirFd = -1;
    if (!openPresetDirFd(presetDirectory(), true, dirFd, &err)) {
        emit errorOccurred(err);
        return false;
    }

    const QByteArray fileNameNative = QFile::encodeName(fileName);
    const QByteArray payload = tomlText.toUtf8();
    const bool written = atomicWriteFileInDir(dirFd, fileNameNative, payload, &err);
    (void)closeFd(dirFd);

    if (!written) {
        emit errorOccurred(err);
        return false;
    }

    emit presetSaved(preset.id);
    return true;
}

bool PresetStore::loadPresetInternal(const QString &id, WallpaperPreset &outPreset,
                                     QString *errorOut) const
{
    const QString safeId = sanitizeId(id);
    const QString fileName = safeId + QStringLiteral(".toml");
    if (!validateConfinedPresetFileName(fileName, errorOut)) {
        return false;
    }

    int dirFd = -1;
    if (!openPresetDirFd(presetDirectory(), false, dirFd, errorOut)) {
        return false;
    }

    QByteArray rawBytes;
    const bool readOk =
        readRegularFileInDir(dirFd, QFile::encodeName(fileName), rawBytes, errorOut);
    (void)closeFd(dirFd);
    if (!readOk) {
        return false;
    }

    WallpaperPreset preset{};
    preset.id = safeId;

    const QString content = QString::fromUtf8(rawBytes);
    const QStringList lines = content.split(QLatin1Char('\n'));
    QString currentSection;
    std::map<QString, QString> uniformTypes;

    auto failLoad = [errorOut](const QString &msg) -> bool {
        if (errorOut) {
            *errorOut = msg;
        }
        return false;
    };

    for (const QString &rawLine : lines) {
        QString line = rawLine.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            continue;
        }

        if (line.startsWith(QLatin1Char('['))) {
            const qsizetype closeIdx = line.indexOf(QLatin1Char(']'));
            if (closeIdx > 1) {
                const QString trailing = line.mid(closeIdx + 1).trimmed();
                if (trailing.isEmpty() || trailing.startsWith(QLatin1Char('#'))) {
                    currentSection = line.mid(1, closeIdx - 1).trimmed().toLower();
                    continue;
                }
            }
            return failLoad(QStringLiteral("Malformed TOML section header in preset: ") + line);
        }

        QString key;
        QString val;
        QString unquoted;
        bool isString = false;
        bool stringValid = true;
        if (!parseTomlKeyValueLine(line, key, val, unquoted, isString, stringValid)) {
            return failLoad(QStringLiteral("Malformed TOML key-value line in preset: ") + line);
        }
        if (isString && !stringValid) {
            return failLoad(QStringLiteral("Malformed TOML string or escape for key: ") + key);
        }

        auto parseBool = [](const QString &v, bool &outBool) -> bool {
            const QString low = v.toLower();
            if (low == QStringLiteral("true") || v == QStringLiteral("1")) {
                outBool = true;
                return true;
            }
            if (low == QStringLiteral("false") || v == QStringLiteral("0")) {
                outBool = false;
                return true;
            }
            return false;
        };

        if (currentSection == QStringLiteral("preset")) {
            if (!isString) {
                return failLoad(QStringLiteral("Expected string value in [preset] for key: ") +
                                key);
            }
            if (key == QStringLiteral("id")) {
                preset.id = sanitizeId(unquoted);
            } else if (key == QStringLiteral("title")) {
                preset.title = unquoted;
            } else if (key == QStringLiteral("wallpaper_path")) {
                preset.wallpaperPath = unquoted;
            }
        } else if (currentSection == QStringLiteral("transition")) {
            if (key == QStringLiteral("type")) {
                if (!isString) {
                    return failLoad(QStringLiteral("Invalid [transition] type string"));
                }
                preset.transitionType = unquoted;
            } else if (key == QStringLiteral("duration_ms")) {
                uint32_t dur = 0;
                if (!parseUInt32(val, dur)) {
                    return failLoad(QStringLiteral("Invalid [transition] duration_ms: ") + val);
                }
                preset.transitionDurationMs = dur;
            } else if (key == QStringLiteral("fps")) {
                uint32_t fps = 0;
                if (!parseUInt32(val, fps)) {
                    return failLoad(QStringLiteral("Invalid [transition] fps: ") + val);
                }
                preset.transitionFps = fps;
            } else if (key == QStringLiteral("has_custom_bezier")) {
                bool b = false;
                if (!parseBool(val, b)) {
                    return failLoad(QStringLiteral("Invalid [transition] has_custom_bezier: ") +
                                    val);
                }
                preset.hasCustomBezier = b;
            } else if (key == QStringLiteral("bezier")) {
                std::vector<float> b;
                if (!parseNumberArray(val, b) || b.size() != 4) {
                    return failLoad(QStringLiteral("Invalid [transition] bezier control points"));
                }
                preset.bezier = bezier_create(b[0], b[1], b[2], b[3]);
            }
        } else if (currentSection == QStringLiteral("power")) {
            if (key == QStringLiteral("battery_policy")) {
                if (!isString) {
                    return failLoad(QStringLiteral("Invalid [power] battery_policy string"));
                }
                preset.batteryPolicy = unquoted;
            } else if (key == QStringLiteral("mute_on_audio")) {
                bool b = false;
                if (!parseBool(val, b)) {
                    return failLoad(QStringLiteral("Invalid [power] mute_on_audio: ") + val);
                }
                preset.muteOnAudio = b;
            } else if (key == QStringLiteral("volume")) {
                float v = 0.0f;
                if (!parseFiniteFloat(val, v)) {
                    return failLoad(QStringLiteral("Invalid or non-finite [power] volume: ") + val);
                }
                preset.volume = v;
            }
        } else if (currentSection == QStringLiteral("layout")) {
            if (key == QStringLiteral("crop_gravity_x")) {
                float v = 0.0f;
                if (!parseFiniteFloat(val, v)) {
                    return failLoad(QStringLiteral("Invalid [layout] crop_gravity_x: ") + val);
                }
                preset.cropGravityX = v;
            } else if (key == QStringLiteral("crop_gravity_y")) {
                float v = 0.0f;
                if (!parseFiniteFloat(val, v)) {
                    return failLoad(QStringLiteral("Invalid [layout] crop_gravity_y: ") + val);
                }
                preset.cropGravityY = v;
            } else if (key == QStringLiteral("has_crop_gravity")) {
                bool b = false;
                if (!parseBool(val, b)) {
                    return failLoad(QStringLiteral("Invalid [layout] has_crop_gravity: ") + val);
                }
                preset.hasCropGravity = b;
            }
        } else if (currentSection == QStringLiteral("uniforms")) {
            if (val.startsWith(QLatin1Char('['))) {
                std::vector<float> nums;
                if (!parseNumberArray(val, nums)) {
                    return failLoad(QStringLiteral("Invalid numeric array for uniform: ") + key);
                }
                if (nums.size() == 1) {
                    preset.uniforms[key] = UniformValue::fromFloat(nums[0]);
                } else if (nums.size() == 2) {
                    preset.uniforms[key] = UniformValue::fromVec2(nums[0], nums[1]);
                } else if (nums.size() == 3) {
                    preset.uniforms[key] = UniformValue::fromVec3(nums[0], nums[1], nums[2]);
                } else if (nums.size() >= 4) {
                    preset.uniforms[key] =
                        UniformValue::fromVec4(nums[0], nums[1], nums[2], nums[3]);
                }
            } else if (isString) {
                preset.uniforms[key] = UniformValue::fromString(unquoted);
            } else if (val == QStringLiteral("true") || val == QStringLiteral("false")) {
                preset.uniforms[key] = UniformValue::fromBool(val == QStringLiteral("true"));
            } else if (val.contains(QLatin1Char('.')) || val.contains(QLatin1Char('e')) ||
                       val.contains(QLatin1Char('E'))) {
                float fVal = 0.0f;
                if (!parseFiniteFloat(val, fVal)) {
                    return failLoad(QStringLiteral("Invalid float for uniform ") + key +
                                    QStringLiteral(": ") + val);
                }
                preset.uniforms[key] = UniformValue::fromFloat(fVal);
            } else {
                bool isInt = false;
                const qlonglong intVal = val.toLongLong(&isInt, 10);
                if (!isInt || intVal < std::numeric_limits<int32_t>::min() ||
                    intVal > std::numeric_limits<int32_t>::max()) {
                    return failLoad(QStringLiteral("Invalid scalar value for uniform ") + key +
                                    QStringLiteral(": ") + val);
                }
                preset.uniforms[key] = UniformValue::fromInt(static_cast<int32_t>(intVal));
            }
        } else if (currentSection == QStringLiteral("uniform_types")) {
            if (!isString) {
                return failLoad(QStringLiteral("Invalid [uniform_types] string for key: ") + key);
            }
            uniformTypes[key] = unquoted.trimmed().toLower();
        } else if (currentSection == QStringLiteral("extra")) {
            if (!isString) {
                return failLoad(QStringLiteral("Invalid [extra] string for key: ") + key);
            }
            preset.extraProperties[key] = unquoted;
        }
    }

    for (const auto &[key, typeName] : uniformTypes) {
        if (typeName != QStringLiteral("color")) {
            continue;
        }
        auto uIt = preset.uniforms.find(key);
        if (uIt == preset.uniforms.end()) {
            continue;
        }
        if (uIt->second.type == UniformValue::Type::Vec3) {
            uIt->second.floats[3] = 1.0f;
            uIt->second.type = UniformValue::Type::Color;
        } else if (uIt->second.type == UniformValue::Type::Vec4) {
            uIt->second.type = UniformValue::Type::Color;
        }
        if (uIt->second.type == UniformValue::Type::Color) {
            const float r = std::clamp(uIt->second.floats[0], 0.0f, 1.0f);
            const float g = std::clamp(uIt->second.floats[1], 0.0f, 1.0f);
            const float b = std::clamp(uIt->second.floats[2], 0.0f, 1.0f);
            const float a = std::clamp(uIt->second.floats[3], 0.0f, 1.0f);
            uIt->second.stringVal = QColor::fromRgbF(r, g, b, a).name(QColor::HexRgb);
        }
    }

    outPreset = std::move(preset);
    return true;
}

bool PresetStore::loadPreset(const QString &id, WallpaperPreset &outPreset)
{
    QString err;
    if (!loadPresetInternal(id, outPreset, &err)) {
        emit errorOccurred(err);
        return false;
    }
    return true;
}

} // namespace waywal::studio
