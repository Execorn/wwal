// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/theme/template_exporter.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace waywal::studio {
namespace {

enum class SlotGroup : uint8_t {
    M3Core,
    M3Extended,
    Term,
    Catppuccin,
    Kde,
    SurfaceAlt,
};

struct PaletteSlot {
    const char *camelName;
    const char *snakeName; // Populated only for tokens emitted by Hyprland and Waybar
    color_rgba_t waywal_palette_t::*member;
    int termIndex; // >= 0 indexes pal.term[]; avoids pointer-to-anonymous-union-member
    SlotGroup group;
};

constexpr std::array<PaletteSlot, 110> kPaletteSlots = {{
    // Core Material 3 roles (also emitted as Hyprland/Waybar variables)
    {"primary", "primary", &waywal_palette_t::primary, -1, SlotGroup::M3Core},
    {"onPrimary", "on_primary", &waywal_palette_t::onPrimary, -1, SlotGroup::M3Core},
    {"primaryContainer", "primary_container", &waywal_palette_t::primaryContainer, -1,
     SlotGroup::M3Core},
    {"onPrimaryContainer", "on_primary_container", &waywal_palette_t::onPrimaryContainer, -1,
     SlotGroup::M3Core},
    {"inversePrimary", "inverse_primary", &waywal_palette_t::inversePrimary, -1, SlotGroup::M3Core},

    {"secondary", "secondary", &waywal_palette_t::secondary, -1, SlotGroup::M3Core},
    {"onSecondary", "on_secondary", &waywal_palette_t::onSecondary, -1, SlotGroup::M3Core},
    {"secondaryContainer", "secondary_container", &waywal_palette_t::secondaryContainer, -1,
     SlotGroup::M3Core},
    {"onSecondaryContainer", "on_secondary_container", &waywal_palette_t::onSecondaryContainer, -1,
     SlotGroup::M3Core},

    {"tertiary", "tertiary", &waywal_palette_t::tertiary, -1, SlotGroup::M3Core},
    {"onTertiary", "on_tertiary", &waywal_palette_t::onTertiary, -1, SlotGroup::M3Core},
    {"tertiaryContainer", "tertiary_container", &waywal_palette_t::tertiaryContainer, -1,
     SlotGroup::M3Core},
    {"onTertiaryContainer", "on_tertiary_container", &waywal_palette_t::onTertiaryContainer, -1,
     SlotGroup::M3Core},

    {"error", "error", &waywal_palette_t::error, -1, SlotGroup::M3Core},
    {"onError", "on_error", &waywal_palette_t::onError, -1, SlotGroup::M3Core},
    {"errorContainer", "error_container", &waywal_palette_t::errorContainer, -1, SlotGroup::M3Core},
    {"onErrorContainer", "on_error_container", &waywal_palette_t::onErrorContainer, -1,
     SlotGroup::M3Core},

    {"background", "background", &waywal_palette_t::background, -1, SlotGroup::M3Core},
    {"onBackground", "on_background", &waywal_palette_t::onBackground, -1, SlotGroup::M3Core},
    {"surface", "surface", &waywal_palette_t::surface, -1, SlotGroup::M3Core},
    {"onSurface", "on_surface", &waywal_palette_t::onSurface, -1, SlotGroup::M3Core},
    {"surfaceDim", "surface_dim", &waywal_palette_t::surfaceDim, -1, SlotGroup::M3Core},
    {"surfaceBright", "surface_bright", &waywal_palette_t::surfaceBright, -1, SlotGroup::M3Core},
    {"surfaceVariant", "surface_variant", &waywal_palette_t::surfaceVariant, -1, SlotGroup::M3Core},
    {"onSurfaceVariant", "on_surface_variant", &waywal_palette_t::onSurfaceVariant, -1,
     SlotGroup::M3Core},
    {"surfaceContainerLowest", "surface_container_lowest",
     &waywal_palette_t::surfaceContainerLowest, -1, SlotGroup::M3Core},
    {"surfaceContainerLow", "surface_container_low", &waywal_palette_t::surfaceContainerLow, -1,
     SlotGroup::M3Core},
    {"surfaceContainer", "surface_container", &waywal_palette_t::surfaceContainer, -1,
     SlotGroup::M3Core},
    {"surfaceContainerHigh", "surface_container_high", &waywal_palette_t::surfaceContainerHigh, -1,
     SlotGroup::M3Core},
    {"surfaceContainerHighest", "surface_container_highest",
     &waywal_palette_t::surfaceContainerHighest, -1, SlotGroup::M3Core},

    {"outline", "outline", &waywal_palette_t::outline, -1, SlotGroup::M3Core},
    {"outlineVariant", "outline_variant", &waywal_palette_t::outlineVariant, -1, SlotGroup::M3Core},
    {"shadow", "shadow", &waywal_palette_t::shadow, -1, SlotGroup::M3Core},

    // Extended Material 3 key, surface, fixed, and semantic roles
    {"primary_paletteKeyColor", nullptr, &waywal_palette_t::primary_paletteKeyColor, -1,
     SlotGroup::M3Extended},
    {"secondary_paletteKeyColor", nullptr, &waywal_palette_t::secondary_paletteKeyColor, -1,
     SlotGroup::M3Extended},
    {"tertiary_paletteKeyColor", nullptr, &waywal_palette_t::tertiary_paletteKeyColor, -1,
     SlotGroup::M3Extended},
    {"neutral_paletteKeyColor", nullptr, &waywal_palette_t::neutral_paletteKeyColor, -1,
     SlotGroup::M3Extended},
    {"neutral_variant_paletteKeyColor", nullptr, &waywal_palette_t::neutral_variant_paletteKeyColor,
     -1, SlotGroup::M3Extended},

    {"inverseSurface", nullptr, &waywal_palette_t::inverseSurface, -1, SlotGroup::M3Extended},
    {"inverseOnSurface", nullptr, &waywal_palette_t::inverseOnSurface, -1, SlotGroup::M3Extended},
    {"scrim", nullptr, &waywal_palette_t::scrim, -1, SlotGroup::M3Extended},
    {"surfaceTint", nullptr, &waywal_palette_t::surfaceTint, -1, SlotGroup::M3Extended},

    {"primaryFixed", nullptr, &waywal_palette_t::primaryFixed, -1, SlotGroup::M3Extended},
    {"primaryFixedDim", nullptr, &waywal_palette_t::primaryFixedDim, -1, SlotGroup::M3Extended},
    {"onPrimaryFixed", nullptr, &waywal_palette_t::onPrimaryFixed, -1, SlotGroup::M3Extended},
    {"onPrimaryFixedVariant", nullptr, &waywal_palette_t::onPrimaryFixedVariant, -1,
     SlotGroup::M3Extended},
    {"secondaryFixed", nullptr, &waywal_palette_t::secondaryFixed, -1, SlotGroup::M3Extended},
    {"secondaryFixedDim", nullptr, &waywal_palette_t::secondaryFixedDim, -1, SlotGroup::M3Extended},
    {"onSecondaryFixed", nullptr, &waywal_palette_t::onSecondaryFixed, -1, SlotGroup::M3Extended},
    {"onSecondaryFixedVariant", nullptr, &waywal_palette_t::onSecondaryFixedVariant, -1,
     SlotGroup::M3Extended},
    {"tertiaryFixed", nullptr, &waywal_palette_t::tertiaryFixed, -1, SlotGroup::M3Extended},
    {"tertiaryFixedDim", nullptr, &waywal_palette_t::tertiaryFixedDim, -1, SlotGroup::M3Extended},
    {"onTertiaryFixed", nullptr, &waywal_palette_t::onTertiaryFixed, -1, SlotGroup::M3Extended},
    {"onTertiaryFixedVariant", nullptr, &waywal_palette_t::onTertiaryFixedVariant, -1,
     SlotGroup::M3Extended},

    {"success", nullptr, &waywal_palette_t::success, -1, SlotGroup::M3Extended},
    {"onSuccess", nullptr, &waywal_palette_t::onSuccess, -1, SlotGroup::M3Extended},
    {"successContainer", nullptr, &waywal_palette_t::successContainer, -1, SlotGroup::M3Extended},
    {"onSuccessContainer", nullptr, &waywal_palette_t::onSuccessContainer, -1,
     SlotGroup::M3Extended},

    // Terminal ANSI 0-15 slots
    {"term0", nullptr, nullptr, 0, SlotGroup::Term},
    {"term1", nullptr, nullptr, 1, SlotGroup::Term},
    {"term2", nullptr, nullptr, 2, SlotGroup::Term},
    {"term3", nullptr, nullptr, 3, SlotGroup::Term},
    {"term4", nullptr, nullptr, 4, SlotGroup::Term},
    {"term5", nullptr, nullptr, 5, SlotGroup::Term},
    {"term6", nullptr, nullptr, 6, SlotGroup::Term},
    {"term7", nullptr, nullptr, 7, SlotGroup::Term},
    {"term8", nullptr, nullptr, 8, SlotGroup::Term},
    {"term9", nullptr, nullptr, 9, SlotGroup::Term},
    {"term10", nullptr, nullptr, 10, SlotGroup::Term},
    {"term11", nullptr, nullptr, 11, SlotGroup::Term},
    {"term12", nullptr, nullptr, 12, SlotGroup::Term},
    {"term13", nullptr, nullptr, 13, SlotGroup::Term},
    {"term14", nullptr, nullptr, 14, SlotGroup::Term},
    {"term15", nullptr, nullptr, 15, SlotGroup::Term},

    // Catppuccin palette tokens
    {"rosewater", nullptr, &waywal_palette_t::rosewater, -1, SlotGroup::Catppuccin},
    {"flamingo", nullptr, &waywal_palette_t::flamingo, -1, SlotGroup::Catppuccin},
    {"pink", nullptr, &waywal_palette_t::pink, -1, SlotGroup::Catppuccin},
    {"mauve", nullptr, &waywal_palette_t::mauve, -1, SlotGroup::Catppuccin},
    {"red", nullptr, &waywal_palette_t::red, -1, SlotGroup::Catppuccin},
    {"maroon", nullptr, &waywal_palette_t::maroon, -1, SlotGroup::Catppuccin},
    {"peach", nullptr, &waywal_palette_t::peach, -1, SlotGroup::Catppuccin},
    {"yellow", nullptr, &waywal_palette_t::yellow, -1, SlotGroup::Catppuccin},
    {"green", nullptr, &waywal_palette_t::green, -1, SlotGroup::Catppuccin},
    {"teal", nullptr, &waywal_palette_t::teal, -1, SlotGroup::Catppuccin},
    {"sky", nullptr, &waywal_palette_t::sky, -1, SlotGroup::Catppuccin},
    {"sapphire", nullptr, &waywal_palette_t::sapphire, -1, SlotGroup::Catppuccin},
    {"blue", nullptr, &waywal_palette_t::blue, -1, SlotGroup::Catppuccin},
    {"lavender", nullptr, &waywal_palette_t::lavender, -1, SlotGroup::Catppuccin},

    // KDE semantic roles
    {"klink", nullptr, &waywal_palette_t::klink, -1, SlotGroup::Kde},
    {"klinkSelection", nullptr, &waywal_palette_t::klinkSelection, -1, SlotGroup::Kde},
    {"kvisited", nullptr, &waywal_palette_t::kvisited, -1, SlotGroup::Kde},
    {"kvisitedSelection", nullptr, &waywal_palette_t::kvisitedSelection, -1, SlotGroup::Kde},
    {"knegative", nullptr, &waywal_palette_t::knegative, -1, SlotGroup::Kde},
    {"knegativeSelection", nullptr, &waywal_palette_t::knegativeSelection, -1, SlotGroup::Kde},
    {"kneutral", nullptr, &waywal_palette_t::kneutral, -1, SlotGroup::Kde},
    {"kneutralSelection", nullptr, &waywal_palette_t::kneutralSelection, -1, SlotGroup::Kde},
    {"kpositive", nullptr, &waywal_palette_t::kpositive, -1, SlotGroup::Kde},
    {"kpositiveSelection", nullptr, &waywal_palette_t::kpositiveSelection, -1, SlotGroup::Kde},

    // Catppuccin-style surface hierarchy tokens
    {"text", nullptr, &waywal_palette_t::text, -1, SlotGroup::SurfaceAlt},
    {"subtext1", nullptr, &waywal_palette_t::subtext1, -1, SlotGroup::SurfaceAlt},
    {"subtext0", nullptr, &waywal_palette_t::subtext0, -1, SlotGroup::SurfaceAlt},
    {"overlay2", nullptr, &waywal_palette_t::overlay2, -1, SlotGroup::SurfaceAlt},
    {"overlay1", nullptr, &waywal_palette_t::overlay1, -1, SlotGroup::SurfaceAlt},
    {"overlay0", nullptr, &waywal_palette_t::overlay0, -1, SlotGroup::SurfaceAlt},
    {"surface2", nullptr, &waywal_palette_t::surface2, -1, SlotGroup::SurfaceAlt},
    {"surface1", nullptr, &waywal_palette_t::surface1, -1, SlotGroup::SurfaceAlt},
    {"surface0", nullptr, &waywal_palette_t::surface0, -1, SlotGroup::SurfaceAlt},
    {"base", nullptr, &waywal_palette_t::base, -1, SlotGroup::SurfaceAlt},
    {"mantle", nullptr, &waywal_palette_t::mantle, -1, SlotGroup::SurfaceAlt},
    {"crust", nullptr, &waywal_palette_t::crust, -1, SlotGroup::SurfaceAlt},
}};

[[nodiscard]] color_rgba_t readSlot(const waywal_palette_t &pal, const PaletteSlot &slot) noexcept
{
    if (slot.termIndex >= 0 && slot.termIndex < 16) {
        return pal.term[slot.termIndex];
    }
    return pal.*(slot.member);
}

[[nodiscard]] const char *caelestiaVariantString(waywal_scheme_variant_t variant) noexcept
{
    switch (variant) {
    case WAYWAL_SCHEME_VIBRANT:
        return "vibrant";
    case WAYWAL_SCHEME_EXPRESSIVE:
        return "expressive";
    case WAYWAL_SCHEME_CONTENT:
        return "content";
    case WAYWAL_SCHEME_FIDELITY:
        return "fidelity";
    case WAYWAL_SCHEME_NEUTRAL:
        return "neutral";
    case WAYWAL_SCHEME_MONOCHROME:
        return "monochrome";
    case WAYWAL_SCHEME_RAINBOW:
        return "rainbow";
    case WAYWAL_SCHEME_FRUIT_SALAD:
        return "fruitsalad";
    case WAYWAL_SCHEME_TONAL_SPOT:
    default:
        return "tonalspot";
    }
}

[[nodiscard]] double sanitizeContrast(float rawContrast) noexcept
{
    double contrast = static_cast<double>(rawContrast);
    if (!std::isfinite(contrast)) {
        return 0.0;
    }
    contrast = std::clamp(contrast, -1.0, 1.0);
    // Avoid serializing -0.0 into JSON outputs.
    return (contrast == 0.0) ? 0.0 : contrast;
}

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

[[nodiscard]] bool closeFd(int fd) noexcept
{
    // On Linux close() always releases the file descriptor, so never retry on EINTR.
    return ::close(fd) == 0 || errno == EINTR;
}

[[nodiscard]] bool fsyncParentDir(const QString &dirPath, QString *errorOut)
{
    const QByteArray dirNative = QFile::encodeName(dirPath);
    int dirFd = -1;
    do {
        dirFd = ::open(dirNative.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    } while (dirFd < 0 && errno == EINTR);

    if (dirFd < 0) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to open parent directory for sync: ") + dirPath +
                        QStringLiteral(" (") + QString::fromLocal8Bit(std::strerror(errno)) +
                        QStringLiteral(")");
        }
        return false;
    }

    bool ok = true;
    int savedErr = 0;
    while (::fsync(dirFd) != 0) {
        if (errno == EINTR) {
            continue;
        }
        if (errno == EINVAL || errno == ENOTSUP) {
            // Virtual filesystems such as tmpfs/FUSE variants may reject directory fsync.
            break;
        }
        ok = false;
        savedErr = errno;
        break;
    }

    if (!closeFd(dirFd) && ok) {
        ok = false;
        savedErr = errno;
    }
    if (!ok && errorOut) {
        *errorOut = QStringLiteral("Directory sync failed for ") + dirPath +
                    QStringLiteral(" (") + QString::fromLocal8Bit(std::strerror(savedErr)) +
                    QStringLiteral(")");
    }
    return ok;
}

[[nodiscard]] bool resolveSymlinkTarget(const QString &rawTargetPath, QString &writePathOut,
                                        QString *errorOut)
{
    if (rawTargetPath.isEmpty() || rawTargetPath.endsWith(QLatin1Char('/'))) {
        if (errorOut) {
            *errorOut = QStringLiteral("Invalid target file path: ") + rawTargetPath;
        }
        return false;
    }

    QString cur = QDir::cleanPath(QFileInfo(rawTargetPath).absoluteFilePath());
    for (int depth = 0; depth < 16; ++depth) {
        const QByteArray curNative = QFile::encodeName(cur);
        struct stat st {};
        if (::lstat(curNative.constData(), &st) != 0) {
            const int err = errno;
            if (err == ENOENT) {
                writePathOut = cur;
                return true;
            }
            if (errorOut) {
                *errorOut = QStringLiteral("Cannot inspect target path ") + cur +
                            QStringLiteral(" (") + QString::fromLocal8Bit(std::strerror(err)) +
                            QStringLiteral(")");
            }
            return false;
        }

        if (S_ISDIR(st.st_mode)) {
            if (errorOut) {
                *errorOut = QStringLiteral("Target path is a directory: ") + cur;
            }
            return false;
        }

        if (!S_ISLNK(st.st_mode)) {
            if (!S_ISREG(st.st_mode)) {
                if (errorOut) {
                    *errorOut = QStringLiteral("Target path is not a regular file: ") + cur;
                }
                return false;
            }
            writePathOut = cur;
            return true;
        }

        // Preserve user dotfile symlinks by resolving to their backing regular file before
        // rename() instead of overwriting the symlink entry itself.
        if (char *rp = ::realpath(curNative.constData(), nullptr)) {
            const QString resolved = QFile::decodeName(rp);
            std::free(rp);
            struct stat targetSt {};
            if (::lstat(QFile::encodeName(resolved).constData(), &targetSt) == 0) {
                if (S_ISDIR(targetSt.st_mode)) {
                    if (errorOut) {
                        *errorOut = QStringLiteral("Symlink resolves to a directory: ") + resolved;
                    }
                    return false;
                }
                if (!S_ISREG(targetSt.st_mode)) {
                    if (errorOut) {
                        *errorOut =
                            QStringLiteral("Symlink resolves to a non-regular file: ") + resolved;
                    }
                    return false;
                }
            }
            writePathOut = resolved;
            return true;
        }

        std::array<char, 4096> linkBuf{};
        const ssize_t linkLen = ::readlink(curNative.constData(), linkBuf.data(), linkBuf.size());
        if (linkLen <= 0 || static_cast<size_t>(linkLen) >= linkBuf.size()) {
            if (errorOut) {
                *errorOut = QStringLiteral("Failed to read symlink target: ") + cur;
            }
            return false;
        }

        const QString linkTarget =
            QFile::decodeName(QByteArray(linkBuf.data(), static_cast<int>(linkLen)));
        if (QDir::isAbsolutePath(linkTarget)) {
            cur = QDir::cleanPath(linkTarget);
        } else {
            const QString parentPath = QFileInfo(cur).dir().absolutePath();
            const QByteArray parentNative = QFile::encodeName(parentPath);
            if (char *parentReal = ::realpath(parentNative.constData(), nullptr)) {
                const QString realParent = QFile::decodeName(parentReal);
                std::free(parentReal);
                cur = QDir::cleanPath(QDir(realParent).filePath(linkTarget));
            } else {
                cur = QDir::cleanPath(QDir(parentPath).filePath(linkTarget));
            }
        }
    }

    if (errorOut) {
        *errorOut = QStringLiteral("Too many levels of symbolic links: ") + rawTargetPath;
    }
    return false;
}

[[nodiscard]] bool createAndWriteTempFile(const QString &writePath, const QByteArray &bytes,
                                          QString &tempPathOut, QString *errorOut)
{
    static std::atomic<quint64> s_seq{0};
    const qint64 pid = QCoreApplication::applicationPid();
    const QFileInfo writeInfo(writePath);
    const QDir parentDir = writeInfo.dir();
    const bool shortName = (writeInfo.fileName().toUtf8().size() <= 180);

    for (int attempt = 0; attempt < 16; ++attempt) {
        const quint64 seq = s_seq.fetch_add(1, std::memory_order_relaxed);
        const quint32 rnd = QRandomGenerator::global()->generate();
        const QString candidate =
            shortName
                ? (writePath + QStringLiteral(".tmp.%1.%2.%3").arg(pid).arg(seq).arg(rnd))
                : parentDir.filePath(
                      QStringLiteral(".wwal.tmp.%1.%2.%3").arg(pid).arg(seq).arg(rnd));
        const QByteArray candidateNative = QFile::encodeName(candidate);

        const int fd = ::open(candidateNative.constData(),
                              O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (fd < 0) {
            if (errno == EEXIST) {
                continue;
            }
            if (errorOut) {
                *errorOut = QStringLiteral("Cannot create temporary file ") + candidate +
                            QStringLiteral(" (") + QString::fromLocal8Bit(std::strerror(errno)) +
                            QStringLiteral(")");
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
            ::unlink(candidateNative.constData());
            if (errorOut) {
                *errorOut = QStringLiteral("Failed writing temporary file ") + candidate +
                            QStringLiteral(" (") + QString::fromLocal8Bit(std::strerror(savedErr)) +
                            QStringLiteral(")");
            }
            return false;
        }

        tempPathOut = candidate;
        return true;
    }

    if (errorOut) {
        *errorOut = QStringLiteral("Exhausted unique temporary file names for ") + writePath;
    }
    return false;
}

[[nodiscard]] bool exdevFallbackWrite(const QString &writePath, const QByteArray &bytes,
                                      QString *errorOut)
{
    const QByteArray writeNative = QFile::encodeName(writePath);
    bool hadOriginal = false;
    QByteArray originalBytes;

    struct stat existingSt {};
    if (::lstat(writeNative.constData(), &existingSt) == 0) {
        if (!S_ISREG(existingSt.st_mode)) {
            if (errorOut) {
                *errorOut =
                    QStringLiteral("Cross-device target is not a regular file: ") + writePath;
            }
            return false;
        }
        if (existingSt.st_size >= 0 && existingSt.st_size <= 4 * 1024 * 1024) {
            QFile orig(writePath);
            if (orig.open(QIODevice::ReadOnly)) {
                originalBytes = orig.readAll();
                hadOriginal = (originalBytes.size() == existingSt.st_size);
            }
        }
    }

    // Open without O_TRUNC and never unlink the target so bind-mounted dotfiles survive EXDEV.
    const int dstFd =
        ::open(writeNative.constData(), O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (dstFd < 0) {
        if (errorOut) {
            *errorOut = QStringLiteral("Cross-device fallback open failed for ") + writePath +
                        QStringLiteral(" (") + QString::fromLocal8Bit(std::strerror(errno)) +
                        QStringLiteral(")");
        }
        return false;
    }

    bool ok = writeAllFd(dstFd, bytes.constData(), static_cast<size_t>(bytes.size()));
    int savedErr = ok ? 0 : errno;
    if (ok && ::ftruncate(dstFd, static_cast<off_t>(bytes.size())) != 0) {
        ok = false;
        savedErr = errno;
    }
    if (ok && !fsyncFd(dstFd)) {
        ok = false;
        savedErr = errno;
    }
    if (!ok && hadOriginal) {
        if (::lseek(dstFd, 0, SEEK_SET) == 0 &&
            writeAllFd(dstFd, originalBytes.constData(),
                       static_cast<size_t>(originalBytes.size())) &&
            ::ftruncate(dstFd, static_cast<off_t>(originalBytes.size())) == 0) {
            (void)fsyncFd(dstFd);
        }
    }
    if (!closeFd(dstFd) && ok) {
        ok = false;
        savedErr = errno;
    }

    if (!ok) {
        if (errorOut) {
            *errorOut = QStringLiteral("Cross-device fallback write failed for ") + writePath +
                        QStringLiteral(" (") + QString::fromLocal8Bit(std::strerror(savedErr)) +
                        QStringLiteral(")");
        }
        return false;
    }

    return fsyncParentDir(QFileInfo(writePath).dir().absolutePath(), errorOut);
}

[[nodiscard]] bool atomicWriteBytes(const QString &targetPath, const QByteArray &bytes,
                                    QString *errorOut)
{
    QString writePath;
    if (!resolveSymlinkTarget(targetPath, writePath, errorOut)) {
        return false;
    }

    const QFileInfo writeInfo(writePath);
    QDir parentDir = writeInfo.dir();
    if ((!parentDir.exists() && !parentDir.mkpath(QStringLiteral("."))) ||
        !QFileInfo(parentDir.absolutePath()).isDir()) {
        if (errorOut) {
            *errorOut =
                QStringLiteral("Failed to create parent directory: ") + parentDir.absolutePath();
        }
        return false;
    }

    QString tempPath;
    if (!createAndWriteTempFile(writePath, bytes, tempPath, errorOut)) {
        return false;
    }

    const QByteArray tempNative = QFile::encodeName(tempPath);
    const QByteArray writeNative = QFile::encodeName(writePath);
    if (::rename(tempNative.constData(), writeNative.constData()) == 0) {
        return fsyncParentDir(parentDir.absolutePath(), errorOut);
    }

    const int renameErr = errno;
    ::unlink(tempNative.constData());
    if (renameErr == EXDEV) {
        return exdevFallbackWrite(writePath, bytes, errorOut);
    }

    if (errorOut) {
        *errorOut = QStringLiteral("Atomic rename failed from ") + tempPath +
                    QStringLiteral(" to ") + writePath + QStringLiteral(" (") +
                    QString::fromLocal8Bit(std::strerror(renameErr)) + QStringLiteral(")");
    }
    return false;
}

[[nodiscard]] const TemplateTargetDescriptor *findDescriptor(TemplateTargetId id)
{
    for (const auto &desc : TemplateExporter::targetDescriptors()) {
        if (desc.id == id) {
            return &desc;
        }
    }
    return nullptr;
}

} // namespace

const QVector<TemplateTargetDescriptor> &TemplateExporter::targetDescriptors()
{
    static const QVector<TemplateTargetDescriptor> descriptors = {
        {TemplateTargetId::Hyprland, QStringLiteral("hyprland"), QStringLiteral("Hyprland"),
         QStringLiteral("hypr/colors.conf"), QString(),
         QStringLiteral("Active border 45 deg gradient, inactive borders, and color variables"),
         true},
        {TemplateTargetId::Waybar, QStringLiteral("waybar"), QStringLiteral("Waybar"),
         QStringLiteral("waybar/colors.css"), QString(),
         QStringLiteral("CSS @define-color tokens for bar modules and pills"), true},
        {TemplateTargetId::Caelestia, QStringLiteral("caelestia"),
         QStringLiteral("Quickshell / Caelestia"),
         QStringLiteral("quickshell/caelestia/colors.json"), QString(),
         QStringLiteral("Full JSON schema for Caelestia Quickshell desktop shell"), true},
        {TemplateTargetId::Kitty, QStringLiteral("kitty"), QStringLiteral("Kitty Terminal"),
         QStringLiteral("kitty/kitty-colors.conf"), QString(),
         QStringLiteral("Foreground, background, cursor, tabs, and 16 ANSI colors"), true},
        {TemplateTargetId::Alacritty, QStringLiteral("alacritty"),
         QStringLiteral("Alacritty Terminal"), QStringLiteral("alacritty/colors.toml"), QString(),
         QStringLiteral("TOML palette with primary, cursor, normal, and bright ANSI colors"), true},
        {TemplateTargetId::RofiFuzzel, QStringLiteral("rofi_fuzzel"),
         QStringLiteral("Rofi & Fuzzel"), QStringLiteral("rofi/colors.rasi"),
         QStringLiteral("fuzzel/colors.ini"),
         QStringLiteral("Application launcher theme definitions (RASI and INI formats)"), true},
        {TemplateTargetId::Mako, QStringLiteral("mako"), QStringLiteral("Mako Notification Daemon"),
         QStringLiteral("mako/config-colors"), QString(),
         QStringLiteral("Notification backgrounds, borders, progress bars, and urgency overrides"),
         true},
        {TemplateTargetId::Gtk3Gtk4, QStringLiteral("gtk"), QStringLiteral("GTK 3.0 & GTK 4.0"),
         QStringLiteral("gtk-3.0/waywal-colors.css"), QStringLiteral("gtk-4.0/waywal-colors.css"),
         QStringLiteral("Universal GTK theme CSS color overrides for Libadwaita & GTK 3"), true}};
    return descriptors;
}

TemplateExporter::TemplateExporter(QWidget *parent) : QWidget(parent) { setupUi(); }

void TemplateExporter::setupUi()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(8);

    auto *headerLabel = new QLabel(QStringLiteral("<b>Config Exporter</b>"), this);
    headerLabel->setStyleSheet(QStringLiteral("font-size: 9.5pt; color: #f8fafc;"));
    mainLayout->addWidget(headerLabel);

    auto *descLabel = new QLabel(QStringLiteral("Write generated color files to ~/.config."), this);
    descLabel->setStyleSheet(QStringLiteral("color: #94a3b8; font-size: 8.5pt;"));
    descLabel->setWordWrap(true);
    mainLayout->addWidget(descLabel);

    // Target Checkbox List
    auto *targetsLayout = new QVBoxLayout();
    targetsLayout->setSpacing(6);

    const auto &descriptors = targetDescriptors();
    m_targets.reserve(descriptors.size());

    for (const auto &desc : descriptors) {
        auto *rowLayout = new QHBoxLayout();
        rowLayout->setContentsMargins(0, 0, 0, 0);

        auto *cb = new QCheckBox(
            QString(desc.displayName).replace(QLatin1String("&"), QLatin1String("&&")), this);
        cb->setChecked(desc.defaultEnabled);
        cb->setToolTip(desc.description);

        QString pathText = QStringLiteral("~/.config/") + desc.primaryPath;
        if (!desc.secondaryPath.isEmpty()) {
            pathText += QStringLiteral(" & ") + desc.secondaryPath;
        }

        auto *pl = new QLabel(pathText, this);
        pl->setStyleSheet(
            QStringLiteral("color: #64748b; font-family: monospace; font-size: 8pt;"));
        pl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        pl->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        pl->setMinimumWidth(50);
        pl->setToolTip(pathText);

        rowLayout->addWidget(cb, 0);
        rowLayout->addWidget(pl, 1);

        targetsLayout->addLayout(rowLayout);

        m_targets.push_back({desc, cb, pl});
    }

    mainLayout->addLayout(targetsLayout);

    // Buttons Row
    auto *btnLayout = new QHBoxLayout();
    btnLayout->setSpacing(8);

    m_selectAllBtn = new QPushButton(QStringLiteral("Select All"), this);
    m_selectAllBtn->setFixedHeight(26);
    connect(m_selectAllBtn, &QPushButton::clicked, this, &TemplateExporter::selectAll);
    btnLayout->addWidget(m_selectAllBtn);

    m_deselectAllBtn = new QPushButton(QStringLiteral("Deselect All"), this);
    m_deselectAllBtn->setFixedHeight(26);
    connect(m_deselectAllBtn, &QPushButton::clicked, this, &TemplateExporter::deselectAll);
    btnLayout->addWidget(m_deselectAllBtn);

    btnLayout->addStretch(1);
    mainLayout->addLayout(btnLayout);

    m_exportBtn = new QPushButton(QStringLiteral("Export Selected to ~/.config"), this);
    m_exportBtn->setFixedHeight(28);
    m_exportBtn->setStyleSheet(
        QStringLiteral("QPushButton { background-color: #0284c7; color: #f8fafc; font-weight: 600; "
                       "padding: 4px 12px; border-radius: 5px; border: 1px solid #0369a1; } "
                       "QPushButton:hover { background-color: #0369a1; } "
                       "QPushButton:pressed { background-color: #075985; }"));
    connect(m_exportBtn, &QPushButton::clicked, this, &TemplateExporter::exportSelectedNow);
    mainLayout->addWidget(m_exportBtn);

    // Status Label
    m_statusLabel = new QLabel(QStringLiteral("Ready to export."), this);
    m_statusLabel->setStyleSheet(QStringLiteral("color: #94a3b8; font-size: 8.5pt;"));
    m_statusLabel->setWordWrap(true);
    mainLayout->addWidget(m_statusLabel);
    mainLayout->addStretch(1);
}

void TemplateExporter::setPalette(const waywal_palette_t &palette)
{
    m_palette = palette;
    m_hasPalette = true;
}

QString TemplateExporter::hexRgb(const color_rgba_t &c)
{
    return QStringLiteral("#") + hexRgbBare(c);
}

QString TemplateExporter::hexRgbBare(const color_rgba_t &c)
{
    return QString::asprintf("%02x%02x%02x", c.r, c.g, c.b);
}

QString TemplateExporter::hexRgba(const color_rgba_t &c)
{
    return QString::asprintf("#%02x%02x%02x%02x", c.r, c.g, c.b, c.a);
}

QString TemplateExporter::hyprRgb(const color_rgba_t &c)
{
    return QString::asprintf("rgb(%02x%02x%02x)", c.r, c.g, c.b);
}

QString TemplateExporter::resolveDestination(const QString &relPath, const QString &overrideBaseDir)
{
    if (relPath.isEmpty()) {
        return QString();
    }
    if (!overrideBaseDir.isEmpty()) {
        return QDir::cleanPath(QDir(overrideBaseDir).filePath(relPath));
    }

    QString configHome = qEnvironmentVariable("XDG_CONFIG_HOME");
    // XDG Base Directory specification mandates ignoring non-absolute XDG_CONFIG_HOME values.
    if (configHome.isEmpty() || !QDir::isAbsolutePath(configHome)) {
        QString home = qEnvironmentVariable("HOME");
        if (home.isEmpty() || !QDir::isAbsolutePath(home)) {
            home = QDir::homePath();
        }
        if (home.isEmpty() || !QDir::isAbsolutePath(home)) {
            return QString();
        }
        configHome = QDir(home).filePath(QStringLiteral(".config"));
    }
    return QDir::cleanPath(QDir(configHome).filePath(relPath));
}

bool TemplateExporter::atomicWrite(const QString &targetPath, const QString &content,
                                   QString *errorOut)
{
    return atomicWriteBytes(targetPath, content.toUtf8(), errorOut);
}

QString TemplateExporter::formatHyprland(const waywal_palette_t &pal)
{
    QString out;
    out += QStringLiteral("# Generated by wwal-studio Material You HCT Live Theme Studio\n");
    out += QStringLiteral("# Auto-generated on ") +
           QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("\n\n");

    for (const auto &slot : kPaletteSlots) {
        if (slot.snakeName == nullptr) {
            continue;
        }
        out += QStringLiteral("$") + QString::fromLatin1(slot.snakeName) + QStringLiteral(" = ") +
               hyprRgb(readSlot(pal, slot)) + QStringLiteral("\n");
    }
    out += QStringLiteral("\n");

    // Standard Hyprland Active and Inactive Border Gradients
    out += QStringLiteral("$active_border = ") + hyprRgb(pal.primary) + QStringLiteral(" ") +
           hyprRgb(pal.tertiary) + QStringLiteral(" 45deg\n");
    out +=
        QStringLiteral("$inactive_border = ") + hyprRgb(pal.outlineVariant) + QStringLiteral("\n");

    return out;
}

QString TemplateExporter::formatWaybar(const waywal_palette_t &pal)
{
    QString out;
    out += QStringLiteral("/* Generated by wwal-studio Material You HCT Live Theme Studio */\n");
    out += QStringLiteral("/* Auto-generated on ") +
           QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral(" */\n\n");

    for (const auto &slot : kPaletteSlots) {
        if (slot.snakeName == nullptr) {
            continue;
        }
        out += QStringLiteral("@define-color ") + QString::fromLatin1(slot.snakeName) +
               QStringLiteral(" ") + hexRgb(readSlot(pal, slot)) + QStringLiteral(";\n");
    }

    return out;
}

QString TemplateExporter::formatCaelestia(const waywal_palette_t &pal)
{
    QJsonObject root;
    root[QStringLiteral("name")] = QStringLiteral("dynamic");
    root[QStringLiteral("flavour")] = QStringLiteral("default");
    root[QStringLiteral("mode")] =
        (pal.mode == WAYWAL_THEME_LIGHT) ? QStringLiteral("light") : QStringLiteral("dark");
    root[QStringLiteral("variant")] = QString::fromLatin1(caelestiaVariantString(pal.variant));
    root[QStringLiteral("generator")] = QStringLiteral("wwal-studio");
    root[QStringLiteral("timestamp")] = QDateTime::currentDateTime().toString(Qt::ISODate);
    root[QStringLiteral("contrast")] = sanitizeContrast(pal.contrast);

    QJsonObject colours;
    QJsonObject colors;
    QJsonObject catppuccin;
    for (const auto &slot : kPaletteSlots) {
        const color_rgba_t c = readSlot(pal, slot);
        const QString key = QString::fromLatin1(slot.camelName);
        colours[key] = hexRgbBare(c);
        const QString prefixed = hexRgb(c);
        colors[key] = prefixed;
        if (slot.group == SlotGroup::Catppuccin) {
            catppuccin[key] = prefixed;
        }
    }
    root[QStringLiteral("colours")] = colours;
    root[QStringLiteral("colors")] = colors;

    // Preserve top-level ANSI array alongside flat term0-15 keys for Studio consumers.
    QJsonArray termArray;
    for (int i = 0; i < 16; ++i) {
        termArray.append(hexRgb(pal.term[i]));
    }
    root[QStringLiteral("term")] = termArray;
    root[QStringLiteral("catppuccin")] = catppuccin;

    const QJsonDocument doc(root);
    return QString::fromUtf8(doc.toJson(QJsonDocument::Indented));
}

QString TemplateExporter::formatKitty(const waywal_palette_t &pal)
{
    QString out;
    out += QStringLiteral("# Generated by wwal-studio Material You HCT Live Theme Studio\n");
    out += QStringLiteral("# Auto-generated on ") +
           QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("\n\n");

    out += QStringLiteral("foreground ") + hexRgb(pal.onSurface) + QStringLiteral("\n");
    out +=
        QStringLiteral("background ") + hexRgb(pal.surfaceContainerLowest) + QStringLiteral("\n");
    out += QStringLiteral("selection_foreground ") + hexRgb(pal.onPrimaryContainer) +
           QStringLiteral("\n");
    out += QStringLiteral("selection_background ") + hexRgb(pal.primaryContainer) +
           QStringLiteral("\n\n");

    out += QStringLiteral("cursor ") + hexRgb(pal.primary) + QStringLiteral("\n");
    out += QStringLiteral("cursor_text_color ") + hexRgb(pal.onPrimary) + QStringLiteral("\n\n");

    out += QStringLiteral("url_color ") + hexRgb(pal.tertiary) + QStringLiteral("\n");
    out += QStringLiteral("active_border_color ") + hexRgb(pal.primary) + QStringLiteral("\n");
    out += QStringLiteral("inactive_border_color ") + hexRgb(pal.outlineVariant) +
           QStringLiteral("\n\n");

    out += QStringLiteral("active_tab_foreground ") + hexRgb(pal.onSurface) + QStringLiteral("\n");
    out += QStringLiteral("active_tab_background ") + hexRgb(pal.surfaceContainerHigh) +
           QStringLiteral("\n");
    out += QStringLiteral("inactive_tab_foreground ") + hexRgb(pal.onSurfaceVariant) +
           QStringLiteral("\n");
    out += QStringLiteral("inactive_tab_background ") + hexRgb(pal.surfaceContainerLow) +
           QStringLiteral("\n\n");

    for (int i = 0; i < 16; ++i) {
        out += QStringLiteral("color") + QString::number(i) + QStringLiteral(" ") +
               hexRgb(pal.term[i]) + QStringLiteral("\n");
    }

    return out;
}

QString TemplateExporter::formatAlacritty(const waywal_palette_t &pal)
{
    QString out;
    out += QStringLiteral("# Generated by wwal-studio Material You HCT Live Theme Studio\n");
    out += QStringLiteral("# Auto-generated on ") +
           QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("\n\n");

    out += QStringLiteral("[colors.primary]\n");
    out += QStringLiteral("background = \"") + hexRgb(pal.surfaceContainerLowest) +
           QStringLiteral("\"\n");
    out += QStringLiteral("foreground = \"") + hexRgb(pal.onSurface) + QStringLiteral("\"\n\n");

    out += QStringLiteral("[colors.cursor]\n");
    out += QStringLiteral("text = \"") + hexRgb(pal.onPrimary) + QStringLiteral("\"\n");
    out += QStringLiteral("cursor = \"") + hexRgb(pal.primary) + QStringLiteral("\"\n\n");

    out += QStringLiteral("[colors.selection]\n");
    out += QStringLiteral("text = \"") + hexRgb(pal.onPrimaryContainer) + QStringLiteral("\"\n");
    out +=
        QStringLiteral("background = \"") + hexRgb(pal.primaryContainer) + QStringLiteral("\"\n\n");

    out += QStringLiteral("[colors.normal]\n");
    out += QStringLiteral("black = \"") + hexRgb(pal.term0) + QStringLiteral("\"\n");
    out += QStringLiteral("red = \"") + hexRgb(pal.term1) + QStringLiteral("\"\n");
    out += QStringLiteral("green = \"") + hexRgb(pal.term2) + QStringLiteral("\"\n");
    out += QStringLiteral("yellow = \"") + hexRgb(pal.term3) + QStringLiteral("\"\n");
    out += QStringLiteral("blue = \"") + hexRgb(pal.term4) + QStringLiteral("\"\n");
    out += QStringLiteral("magenta = \"") + hexRgb(pal.term5) + QStringLiteral("\"\n");
    out += QStringLiteral("cyan = \"") + hexRgb(pal.term6) + QStringLiteral("\"\n");
    out += QStringLiteral("white = \"") + hexRgb(pal.term7) + QStringLiteral("\"\n\n");

    out += QStringLiteral("[colors.bright]\n");
    out += QStringLiteral("black = \"") + hexRgb(pal.term8) + QStringLiteral("\"\n");
    out += QStringLiteral("red = \"") + hexRgb(pal.term9) + QStringLiteral("\"\n");
    out += QStringLiteral("green = \"") + hexRgb(pal.term10) + QStringLiteral("\"\n");
    out += QStringLiteral("yellow = \"") + hexRgb(pal.term11) + QStringLiteral("\"\n");
    out += QStringLiteral("blue = \"") + hexRgb(pal.term12) + QStringLiteral("\"\n");
    out += QStringLiteral("magenta = \"") + hexRgb(pal.term13) + QStringLiteral("\"\n");
    out += QStringLiteral("cyan = \"") + hexRgb(pal.term14) + QStringLiteral("\"\n");
    out += QStringLiteral("white = \"") + hexRgb(pal.term15) + QStringLiteral("\"\n");

    return out;
}

QString TemplateExporter::formatRofi(const waywal_palette_t &pal)
{
    QString out;
    out += QStringLiteral("/* Generated by wwal-studio Material You HCT Live Theme Studio */\n");
    out += QStringLiteral("/* Auto-generated on ") +
           QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral(" */\n\n");

    out += QStringLiteral("* {\n");
    out +=
        QStringLiteral("    primary:              ") + hexRgb(pal.primary) + QStringLiteral(";\n");
    out += QStringLiteral("    on-primary:           ") + hexRgb(pal.onPrimary) +
           QStringLiteral(";\n");
    out += QStringLiteral("    primary-container:    ") + hexRgb(pal.primaryContainer) +
           QStringLiteral(";\n");
    out += QStringLiteral("    on-primary-container: ") + hexRgb(pal.onPrimaryContainer) +
           QStringLiteral(";\n");
    out += QStringLiteral("    secondary:            ") + hexRgb(pal.secondary) +
           QStringLiteral(";\n");
    out += QStringLiteral("    on-secondary:         ") + hexRgb(pal.onSecondary) +
           QStringLiteral(";\n");
    out +=
        QStringLiteral("    tertiary:             ") + hexRgb(pal.tertiary) + QStringLiteral(";\n");
    out += QStringLiteral("    background:           ") + hexRgb(pal.surfaceContainerLowest) +
           QStringLiteral(";\n");
    out += QStringLiteral("    background-alt:       ") + hexRgb(pal.surfaceContainerHigh) +
           QStringLiteral(";\n");
    out += QStringLiteral("    foreground:           ") + hexRgb(pal.onSurface) +
           QStringLiteral(";\n");
    out += QStringLiteral("    selected:             ") + hexRgb(pal.primaryContainer) +
           QStringLiteral(";\n");
    out +=
        QStringLiteral("    active:               ") + hexRgb(pal.primary) + QStringLiteral(";\n");
    out += QStringLiteral("    urgent:               ") + hexRgb(pal.error) + QStringLiteral(";\n");
    out +=
        QStringLiteral("    border-color:         ") + hexRgb(pal.primary) + QStringLiteral(";\n");
    out += QStringLiteral("}\n");

    return out;
}

QString TemplateExporter::formatFuzzel(const waywal_palette_t &pal)
{
    QString out;
    out += QStringLiteral("# Generated by wwal-studio Material You HCT Live Theme Studio\n");
    out += QStringLiteral("# Auto-generated on ") +
           QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("\n\n");

    const auto addFuzzel = [&](const char *name, const color_rgba_t &c, const char *alphaHex) {
        out += QString::fromLatin1(name) + QStringLiteral("=") + hexRgbBare(c) +
               QString::fromLatin1(alphaHex) + QStringLiteral("\n");
    };

    out += QStringLiteral("[colors]\n");
    addFuzzel("background", pal.surfaceContainerLowest, "fa");
    addFuzzel("text", pal.onSurface, "ff");
    addFuzzel("match", pal.primary, "ff");
    addFuzzel("selection", pal.primaryContainer, "ff");
    addFuzzel("selection-text", pal.onPrimaryContainer, "ff");
    addFuzzel("selection-match", pal.tertiary, "ff");
    addFuzzel("border", pal.primary, "ff");

    return out;
}

QString TemplateExporter::formatMako(const waywal_palette_t &pal)
{
    QString out;
    out += QStringLiteral("# Generated by wwal-studio Material You HCT Live Theme Studio\n");
    out += QStringLiteral("# Auto-generated on ") +
           QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("\n\n");

    out +=
        QStringLiteral("background-color=") + hexRgb(pal.surfaceContainer) + QStringLiteral("\n");
    out += QStringLiteral("text-color=") + hexRgb(pal.onSurface) + QStringLiteral("\n");
    out += QStringLiteral("border-color=") + hexRgb(pal.primary) + QStringLiteral("\n");
    out += QStringLiteral("progress-color=over ") + hexRgb(pal.primaryContainer) +
           QStringLiteral("\n\n");

    out += QStringLiteral("[urgency=low]\n");
    out += QStringLiteral("background-color=") + hexRgb(pal.surfaceContainerLow) +
           QStringLiteral("\n");
    out += QStringLiteral("text-color=") + hexRgb(pal.onSurfaceVariant) + QStringLiteral("\n");
    out += QStringLiteral("border-color=") + hexRgb(pal.outlineVariant) + QStringLiteral("\n\n");

    out += QStringLiteral("[urgency=normal]\n");
    out +=
        QStringLiteral("background-color=") + hexRgb(pal.surfaceContainer) + QStringLiteral("\n");
    out += QStringLiteral("text-color=") + hexRgb(pal.onSurface) + QStringLiteral("\n");
    out += QStringLiteral("border-color=") + hexRgb(pal.primary) + QStringLiteral("\n\n");

    out += QStringLiteral("[urgency=high]\n");
    out += QStringLiteral("background-color=") + hexRgb(pal.errorContainer) + QStringLiteral("\n");
    out += QStringLiteral("text-color=") + hexRgb(pal.onErrorContainer) + QStringLiteral("\n");
    out += QStringLiteral("border-color=") + hexRgb(pal.error) + QStringLiteral("\n");

    return out;
}

QString TemplateExporter::formatGtkCss(const waywal_palette_t &pal)
{
    QString out;
    out += QStringLiteral("/* Generated by wwal-studio Material You HCT Live Theme Studio */\n");
    out += QStringLiteral("/* Auto-generated on ") +
           QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral(" */\n\n");

    const auto addCss = [&](const char *name, const color_rgba_t &c) {
        out += QStringLiteral("@define-color ") + QString::fromLatin1(name) + QStringLiteral(" ") +
               hexRgb(c) + QStringLiteral(";\n");
    };

    addCss("accent_color", pal.primary);
    addCss("accent_bg_color", pal.primaryContainer);
    addCss("accent_fg_color", pal.onPrimaryContainer);

    addCss("window_bg_color", pal.surfaceContainerLowest);
    addCss("window_fg_color", pal.onSurface);
    addCss("view_bg_color", pal.surface);
    addCss("view_fg_color", pal.onSurface);

    addCss("headerbar_bg_color", pal.surfaceContainerLow);
    addCss("headerbar_fg_color", pal.onSurface);
    addCss("headerbar_border_color", pal.outlineVariant);

    addCss("card_bg_color", pal.surfaceContainer);
    addCss("card_fg_color", pal.onSurface);
    addCss("dialog_bg_color", pal.surfaceContainerHigh);
    addCss("dialog_fg_color", pal.onSurface);
    addCss("popover_bg_color", pal.surfaceContainerHigh);
    addCss("popover_fg_color", pal.onSurface);

    addCss("destructive_color", pal.error);
    addCss("destructive_bg_color", pal.errorContainer);
    addCss("destructive_fg_color", pal.onErrorContainer);

    addCss("success_color", pal.success);
    addCss("success_bg_color", pal.successContainer);
    addCss("success_fg_color", pal.onSuccessContainer);

    out += QStringLiteral("@define-color shade_color rgba(0, 0, 0, 0.08);\n");
    out += QStringLiteral("@define-color scrollbar_outline_color rgba(0, 0, 0, 0.4);\n");

    return out;
}

bool TemplateExporter::exportTarget(TemplateTargetId id, const QString &overrideBaseDir,
                                    QString *errorOut)
{
    if (!m_hasPalette) {
        if (errorOut) {
            *errorOut = QStringLiteral("Cannot export: palette has not been extracted yet.");
        }
        return false;
    }

    const TemplateTargetDescriptor *desc = findDescriptor(id);
    if (!desc) {
        if (errorOut) {
            *errorOut = QStringLiteral("Unknown export target ID.");
        }
        return false;
    }

    QString primaryContent;
    QString secondaryContent;
    switch (id) {
    case TemplateTargetId::Hyprland:
        primaryContent = formatHyprland(m_palette);
        break;
    case TemplateTargetId::Waybar:
        primaryContent = formatWaybar(m_palette);
        break;
    case TemplateTargetId::Caelestia:
        primaryContent = formatCaelestia(m_palette);
        break;
    case TemplateTargetId::Kitty:
        primaryContent = formatKitty(m_palette);
        break;
    case TemplateTargetId::Alacritty:
        primaryContent = formatAlacritty(m_palette);
        break;
    case TemplateTargetId::RofiFuzzel:
        primaryContent = formatRofi(m_palette);
        secondaryContent = formatFuzzel(m_palette);
        break;
    case TemplateTargetId::Mako:
        primaryContent = formatMako(m_palette);
        break;
    case TemplateTargetId::Gtk3Gtk4:
        primaryContent = formatGtkCss(m_palette);
        secondaryContent = primaryContent;
        break;
    default:
        if (errorOut) {
            *errorOut = QStringLiteral("Unknown export target ID.");
        }
        return false;
    }

    const QString primaryPath = resolveDestination(desc->primaryPath, overrideBaseDir);
    if (primaryPath.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Cannot resolve export destination for ") + desc->primaryPath;
        }
        return false;
    }

    if (desc->secondaryPath.isEmpty()) {
        return atomicWrite(primaryPath, primaryContent, errorOut);
    }

    const QString secondaryPath = resolveDestination(desc->secondaryPath, overrideBaseDir);
    if (secondaryPath.isEmpty()) {
        if (errorOut) {
            *errorOut =
                QStringLiteral("Cannot resolve export destination for ") + desc->secondaryPath;
        }
        return false;
    }

    QString primaryWritePath;
    QString secondaryWritePath;
    if (!resolveSymlinkTarget(primaryPath, primaryWritePath, errorOut) ||
        !resolveSymlinkTarget(secondaryPath, secondaryWritePath, errorOut)) {
        return false;
    }

    bool primaryExisted = false;
    QByteArray primaryBackup;
    struct stat primarySt {};
    const QByteArray primaryNative = QFile::encodeName(primaryWritePath);
    if (::lstat(primaryNative.constData(), &primarySt) == 0) {
        if (primarySt.st_size < 0 || primarySt.st_size > 16 * 1024 * 1024) {
            if (errorOut) {
                *errorOut = QStringLiteral("Existing primary target exceeds backup limit: ") +
                            primaryWritePath;
            }
            return false;
        }
        QFile origFile(primaryWritePath);
        if (!origFile.open(QIODevice::ReadOnly)) {
            if (errorOut) {
                *errorOut = QStringLiteral("Cannot snapshot existing primary target for rollback: ") +
                            primaryWritePath;
            }
            return false;
        }
        primaryBackup = origFile.readAll();
        if (primaryBackup.size() != primarySt.st_size) {
            if (errorOut) {
                *errorOut = QStringLiteral("Incomplete rollback snapshot for: ") + primaryWritePath;
            }
            return false;
        }
        primaryExisted = true;
    }

    if (!atomicWrite(primaryPath, primaryContent, errorOut)) {
        return false;
    }
    if (atomicWrite(secondaryPath, secondaryContent, errorOut)) {
        return true;
    }

    // Roll back the primary file so multi-file exports stay consistent when secondary fails.
    if (primaryExisted) {
        (void)atomicWriteBytes(primaryPath, primaryBackup, nullptr);
    } else {
        (void)::unlink(primaryNative.constData());
    }
    return false;
}

int TemplateExporter::exportSelected(const QString &overrideBaseDir, QStringList *errorsOut)
{
    int successCount = 0;
    for (const auto &item : m_targets) {
        if (item.checkBox && item.checkBox->isChecked()) {
            QString err;
            if (exportTarget(item.desc.id, overrideBaseDir, &err)) {
                ++successCount;
            } else {
                if (errorsOut) {
                    errorsOut->append(item.desc.displayName + QStringLiteral(": ") + err);
                }
            }
        }
    }
    return successCount;
}

int TemplateExporter::exportAll(const QString &overrideBaseDir, QStringList *errorsOut)
{
    int successCount = 0;
    const auto &descriptors = targetDescriptors();
    for (const auto &desc : descriptors) {
        QString err;
        if (exportTarget(desc.id, overrideBaseDir, &err)) {
            ++successCount;
        } else {
            if (errorsOut) {
                errorsOut->append(desc.displayName + QStringLiteral(": ") + err);
            }
        }
    }
    return successCount;
}

bool TemplateExporter::isTargetEnabled(TemplateTargetId id) const
{
    for (const auto &item : m_targets) {
        if (item.desc.id == id) {
            return item.checkBox && item.checkBox->isChecked();
        }
    }
    return false;
}

void TemplateExporter::setTargetEnabled(TemplateTargetId id, bool enabled)
{
    for (auto &item : m_targets) {
        if (item.desc.id == id && item.checkBox) {
            item.checkBox->setChecked(enabled);
            break;
        }
    }
}

void TemplateExporter::selectAll()
{
    for (auto &item : m_targets) {
        if (item.checkBox) {
            item.checkBox->setChecked(true);
        }
    }
}

void TemplateExporter::deselectAll()
{
    for (auto &item : m_targets) {
        if (item.checkBox) {
            item.checkBox->setChecked(false);
        }
    }
}

void TemplateExporter::exportSelectedNow()
{
    if (!m_hasPalette) {
        if (m_statusLabel) {
            m_statusLabel->setText(
                QStringLiteral("<font color='#f44336'>Cannot export: No palette loaded!</font>"));
        }
        return;
    }

    QStringList errors;
    int total = 0;
    for (const auto &item : m_targets) {
        if (item.checkBox && item.checkBox->isChecked()) {
            ++total;
        }
    }

    if (total == 0) {
        if (m_statusLabel) {
            m_statusLabel->setText(QStringLiteral("No export targets selected."));
        }
        return;
    }

    const int successCount = exportSelected(QString(), &errors);
    QString msg;
    if (successCount == total) {
        msg = QStringLiteral("<font color='#4caf50'>Successfully exported %1 targets to "
                             "$XDG_CONFIG_HOME at %2</font>")
                  .arg(successCount)
                  .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")));
    } else {
        msg = QStringLiteral("<font color='#ff9800'>Exported %1 of %2 targets (%3 failed).</font>")
                  .arg(successCount)
                  .arg(total)
                  .arg(errors.size());
    }

    if (m_statusLabel) {
        m_statusLabel->setText(msg);
    }

    Q_EMIT exportCompleted(successCount, total, msg);
}

} // namespace waywal::studio
