// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "studio/queue/download_manager.hpp"
#include "studio/topology/monitor_topology_canvas.hpp"

#include <QAbstractButton>
#include <QComboBox>
#include <QDir>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QLabel>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QSet>
#include <QStandardPaths>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <stdlib.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace wwal::studio::browser_shared {

// High-contrast accessible styles shared across Studio browsers and wizard pages (>= 4.5:1 ratio,
// >= 11px readable font sizes, and visible keyboard focus outlines).
inline constexpr const char *kStylePanelBg = "background-color: #12141a; color: #f1f5f9;";

inline constexpr const char *kStyleToolbarFrame =
    "background-color: #181b22; border-bottom: 1px solid #262b36;";

inline constexpr const char *kStyleSearchBar =
    "QLineEdit { background-color: #1e222b; color: #f8fafc; border: 1px solid #334155; "
    "border-radius: 5px; padding: 4px 10px; font-size: 12px; min-height: 24px; }"
    "QLineEdit:focus { border: 2px solid #38bdf8; }";

inline constexpr const char *kStyleToolbarCombo =
    "QComboBox { background-color: #20242e; color: #f1f5f9; border: 1px solid #334155; "
    "border-radius: 5px; padding: 4px 10px; font-size: 11px; }"
    "QComboBox:hover { border-color: #475569; }"
    "QComboBox:focus { border: 2px solid #38bdf8; }"
    "QComboBox::drop-down { border: none; width: 20px; }"
    "QComboBox QAbstractItemView { background-color: #1a1e26; color: #f1f5f9; "
    "border: 1px solid #334155; selection-background-color: #0369a1; "
    "selection-color: #ffffff; }";

inline constexpr const char *kStyleDrawerBg = "background-color: #181b22; border: none;";

inline constexpr const char *kStyleButtonPrimary =
    "QPushButton, QToolButton { background-color: #0369a1; color: #ffffff; "
    "border: 1px solid #0284c7; border-radius: 5px; padding: 5px 12px; "
    "font-size: 12px; font-weight: 600; }"
    "QPushButton:hover, QToolButton:hover { background-color: #0284c7; }"
    "QPushButton:pressed, QToolButton:pressed { background-color: #075985; }"
    "QPushButton:focus, QToolButton:focus { border: 2px solid #7dd3fc; }"
    "QPushButton:disabled, QToolButton:disabled { background-color: #1e2430; color: #b4c0d0; "
    "border: 1px solid #334155; }";

inline constexpr const char *kStyleButtonEmerald =
    "QPushButton { background-color: #047857; color: #ffffff; border: 1px solid #059669; "
    "border-radius: 5px; padding: 5px 12px; font-size: 12px; font-weight: 600; }"
    "QPushButton:hover { background-color: #059669; }"
    "QPushButton:pressed { background-color: #065f46; }"
    "QPushButton:focus { border: 2px solid #6ee7b7; }"
    "QPushButton:disabled { background-color: #1e2430; color: #b4c0d0; "
    "border: 1px solid #334155; }";

inline constexpr const char *kStyleButtonSecondary =
    "QPushButton, QToolButton { background-color: #20242e; color: #f1f5f9; "
    "border: 1px solid #334155; border-radius: 5px; padding: 5px 12px; font-size: 12px; }"
    "QPushButton:hover, QToolButton:hover { background-color: #2a303d; color: #ffffff; }"
    "QPushButton:pressed, QToolButton:pressed { background-color: #161922; }"
    "QPushButton:focus, QToolButton:focus { border: 2px solid #38bdf8; }"
    "QPushButton:disabled, QToolButton:disabled { background-color: #1a1f29; color: #b4c0d0; "
    "border: 1px solid #2e3545; }";

inline constexpr const char *kStyleButtonDanger =
    "QPushButton { background-color: #b91c1c; color: #ffffff; border: 1px solid #dc2626; "
    "border-radius: 5px; padding: 5px 12px; font-size: 12px; font-weight: 600; }"
    "QPushButton:hover { background-color: #dc2626; }"
    "QPushButton:focus { border: 2px solid #fca5a5; }"
    "QPushButton:disabled { background-color: #1e2430; color: #b4c0d0; "
    "border: 1px solid #334155; }";

inline constexpr const char *kStyleProgressBar =
    "QProgressBar { background: #20242e; border: 1px solid #334155; border-radius: 4px; "
    "text-align: center; font-size: 11px; color: #f8fafc; min-height: 14px; }"
    "QProgressBar::chunk { background-color: #0284c7; border-radius: 3px; }";

inline QString formatBytes(uint64_t bytes)
{
    constexpr double kKiB = 1024.0;
    constexpr double kMiB = kKiB * 1024.0;
    constexpr double kGiB = kMiB * 1024.0;

    if (bytes >= kGiB) {
        return QString::asprintf("%.2f GB", static_cast<double>(bytes) / kGiB);
    }
    if (bytes >= kMiB) {
        return QString::asprintf("%.1f MB", static_cast<double>(bytes) / kMiB);
    }
    if (bytes >= kKiB) {
        return QString::asprintf("%.1f KB", static_cast<double>(bytes) / kKiB);
    }
    return QString::asprintf("%llu B", static_cast<unsigned long long>(bytes));
}

inline QString resolveXdgDataHomeQt()
{
    const QString env = qEnvironmentVariable("XDG_DATA_HOME").trimmed();
    if (!env.isEmpty() && QDir::isAbsolutePath(env)) {
        return QDir::cleanPath(env);
    }
    const QString stdPath = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (!stdPath.isEmpty()) {
        return QDir::cleanPath(stdPath);
    }
    return QDir::homePath() + QStringLiteral("/.local/share");
}

inline QString resolveWaywalDataDirQt(const QString &subdir)
{
    if (subdir.isEmpty()) {
        return resolveXdgDataHomeQt() + QStringLiteral("/waywal");
    }
    return resolveXdgDataHomeQt() + QStringLiteral("/waywal/") + subdir;
}

inline QString resolveWaywalConfigDirQt(const QString &subdir)
{
    const QString env = qEnvironmentVariable("XDG_CONFIG_HOME").trimmed();
    QString base;
    if (!env.isEmpty() && QDir::isAbsolutePath(env)) {
        base = QDir::cleanPath(env);
    } else {
        const QString stdPath =
            QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
        base = !stdPath.isEmpty() ? QDir::cleanPath(stdPath)
                                  : (QDir::homePath() + QStringLiteral("/.config"));
    }
    return subdir.isEmpty() ? (base + QStringLiteral("/waywal"))
                            : (base + QStringLiteral("/waywal/") + subdir);
}

inline void sizeButtonFromFontMetrics(QAbstractButton *btn, int horizontalPad = 16,
                                      int minHeight = 26, bool setMinWidth = true)
{
    if (!btn) {
        return;
    }
    const QFontMetrics fm(btn->font());
    const int targetH = std::max(minHeight, fm.height() + 10);
    btn->setMinimumHeight(targetH);
    if (setMinWidth) {
        const int targetW = fm.horizontalAdvance(btn->text()) + horizontalPad;
        btn->setMinimumWidth(std::max(44, targetW));
    }
}

inline void resetButtonTextLater(QAbstractButton *btn, const QString &temporaryText,
                                 const QString &restoredText, int delayMs = 1600)
{
    if (!btn) {
        return;
    }
    btn->setText(temporaryText);
    QPointer<QAbstractButton> safeBtn(btn);
    QTimer::singleShot(delayMs, btn, [safeBtn, restoredText]() {
        if (safeBtn) {
            safeBtn->setText(restoredText);
        }
    });
}

inline bool writeAllFd(int fd, const void *data, size_t size)
{
    const auto *ptr = static_cast<const uint8_t *>(data);
    size_t written = 0;
    while (written < size) {
        const ssize_t n = ::write(fd, ptr + written, size - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        written += static_cast<size_t>(n);
    }
    return true;
}

inline bool atomicWriteBytesWithFsync(const std::filesystem::path &targetPath, const void *data,
                                      size_t size, mode_t mode = 0644)
{
    if (targetPath.empty()) {
        return false;
    }
    std::error_code ec;
    const std::filesystem::path parentDir = targetPath.parent_path();
    if (!parentDir.empty()) {
        std::filesystem::create_directories(parentDir, ec);
        if (ec) {
            return false;
        }
    }

    std::string tmpl = targetPath.string() + ".tmp.XXXXXX";
    std::vector<char> tmplBuf(tmpl.begin(), tmpl.end());
    tmplBuf.push_back('\0');

    const int fd = ::mkostemp(tmplBuf.data(), O_CLOEXEC);
    if (fd < 0) {
        return false;
    }

    bool ok = (size == 0 || writeAllFd(fd, data, size));
    if (ok) {
        (void)::fchmod(fd, mode);
        if (::fsync(fd) != 0 && errno != EINVAL && errno != ENOTSUP) {
            ok = false;
        }
    }
    if (::close(fd) != 0) {
        ok = false;
    }

    if (!ok) {
        (void)::unlink(tmplBuf.data());
        return false;
    }

    if (::rename(tmplBuf.data(), targetPath.c_str()) != 0) {
        (void)::unlink(tmplBuf.data());
        return false;
    }

    if (!parentDir.empty()) {
        const int dirFd = ::open(parentDir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dirFd >= 0) {
            (void)::fsync(dirFd);
            (void)::close(dirFd);
        }
    }
    return true;
}

inline bool isActiveDownloadPipelineState(TaskState st) noexcept
{
    switch (st) {
    case TaskState::Queued:
    case TaskState::Probing:
    case TaskState::Downloading:
    case TaskState::Extracting:
    case TaskState::Verifying:
    case TaskState::Indexing:
        return true;
    case TaskState::Installed:
    case TaskState::Failed:
    case TaskState::Cancelled:
    default:
        return false;
    }
}

inline void updateDownloadProgressWidgets(QWidget *progressBox, QProgressBar *progressBar,
                                          QLabel *telemetryLabel, TaskState state,
                                          float progressPct, float speedBps, uint64_t totalBytes,
                                          const QString &downloadNoun)
{
    if (!progressBox || !progressBar || !telemetryLabel) {
        return;
    }

    const bool active = isActiveDownloadPipelineState(state);
    progressBox->setVisible(active);
    if (!active) {
        return;
    }

    const float safePct = std::isfinite(progressPct) ? std::clamp(progressPct, 0.0f, 100.0f) : 0.0f;
    const float safeSpeed = (std::isfinite(speedBps) && speedBps > 0.0f) ? speedBps : 0.0f;
    const double speedMiB = static_cast<double>(safeSpeed) / (1024.0 * 1024.0);
    const int barVal = (state == TaskState::Verifying || state == TaskState::Extracting ||
                        state == TaskState::Indexing)
                           ? std::max(99, static_cast<int>(safePct))
                           : static_cast<int>(safePct);
    progressBar->setValue(std::clamp(barVal, 0, 100));

    switch (state) {
    case TaskState::Queued:
        telemetryLabel->setText(QStringLiteral("Queued..."));
        break;
    case TaskState::Probing:
        telemetryLabel->setText(QStringLiteral("Probing source metadata..."));
        break;
    case TaskState::Verifying:
        telemetryLabel->setText(QStringLiteral("Verifying Ed25519 and Merkle integrity..."));
        break;
    case TaskState::Extracting:
        telemetryLabel->setText(QStringLiteral("Extracting package archive..."));
        break;
    case TaskState::Indexing:
        telemetryLabel->setText(QStringLiteral("Indexing wallpaper assets..."));
        break;
    case TaskState::Downloading:
    default: {
        if (totalBytes > 0) {
            const double ratio = static_cast<double>(safePct) / 100.0;
            const uint64_t dlBytes =
                (std::isfinite(ratio) && ratio >= 0.0)
                    ? static_cast<uint64_t>(static_cast<double>(totalBytes) * ratio)
                    : 0;
            uint32_t etaSec = 0;
            if (safeSpeed > 1024.0f && totalBytes > dlBytes) {
                const double rem = static_cast<double>(totalBytes - dlBytes);
                const double etaD = rem / static_cast<double>(safeSpeed);
                if (std::isfinite(etaD) && etaD >= 0.0 && etaD < 864000.0) {
                    etaSec = static_cast<uint32_t>(etaD);
                }
            }
            telemetryLabel->setText(QStringLiteral("%1: %2 / %3 (%4 MB/s, ETA: %5s)")
                                        .arg(downloadNoun)
                                        .arg(formatBytes(dlBytes))
                                        .arg(formatBytes(totalBytes))
                                        .arg(QString::number(speedMiB, 'f', 1))
                                        .arg(etaSec));
        } else {
            telemetryLabel->setText(QStringLiteral("%1: %2% (%3 MB/s)")
                                        .arg(downloadNoun)
                                        .arg(static_cast<int>(safePct))
                                        .arg(QString::number(speedMiB, 'f', 1)));
        }
        break;
    }
    }
}

inline void refreshMonitorTargetCombo(QComboBox *combo, QWidget *ownerWidget)
{
    if (!combo) {
        return;
    }

    const QString prevSelection = combo->currentText();
    QStringList outputs{QStringLiteral("All Displays")};
    QSet<QString> seen;
    seen.insert(QStringLiteral("All Displays"));

    auto addOutputName = [&](const QString &rawName) {
        const QString cleaned = rawName.trimmed();
        if (cleaned.isEmpty() || cleaned == QStringLiteral("*") || seen.contains(cleaned)) {
            return;
        }
        // Keep labels strictly ASCII to satisfy dock widget audit checks.
        for (const QChar &ch : cleaned) {
            if (ch.unicode() > 0x7E) {
                return;
            }
        }
        seen.insert(cleaned);
        outputs.push_back(cleaned);
    };

    if (ownerWidget) {
        if (QWidget *win = ownerWidget->window()) {
            if (auto *canvas = win->findChild<::waywal::studio::MonitorTopologyCanvas *>()) {
                for (const ::waywal::studio::MonitorInfo &m : canvas->monitors()) {
                    addOutputName(m.name);
                }
                if (canvas->property("wwal_combo_hooked").toBool() == false) {
                    canvas->setProperty("wwal_combo_hooked", true);
                }
                const QString hookKey = QStringLiteral("wwal_bound_") +
                                        QString::number(reinterpret_cast<uintptr_t>(combo), 16);
                if (!canvas->property(hookKey.toUtf8().constData()).toBool()) {
                    canvas->setProperty(hookKey.toUtf8().constData(), true);
                    QPointer<QComboBox> safeCombo(combo);
                    QPointer<QWidget> safeOwner(ownerWidget);
                    QObject::connect(canvas,
                                     &::waywal::studio::MonitorTopologyCanvas::outputsRefreshed,
                                     combo, [safeCombo, safeOwner](int) {
                                         if (safeCombo && safeOwner) {
                                             refreshMonitorTargetCombo(safeCombo, safeOwner);
                                         }
                                     });
                }
            }
        }
    }

    if (qGuiApp) {
        for (QScreen *scr : QGuiApplication::screens()) {
            if (scr) {
                addOutputName(scr->name());
            }
        }
    }

    combo->blockSignals(true);
    combo->clear();
    combo->addItems(outputs);
    const int restoreIdx = combo->findText(prevSelection);
    combo->setCurrentIndex(restoreIdx >= 0 ? restoreIdx : 0);
    combo->blockSignals(false);
}

inline void bindMonitorTargetCombo(QComboBox *combo, QWidget *ownerWidget)
{
    if (!combo || !ownerWidget) {
        return;
    }

    refreshMonitorTargetCombo(combo, ownerWidget);

    if (qGuiApp) {
        QPointer<QComboBox> safeCombo(combo);
        QPointer<QWidget> safeOwner(ownerWidget);
        QObject::connect(qGuiApp, &QGuiApplication::screenAdded, combo,
                         [safeCombo, safeOwner](QScreen *) {
                             if (safeCombo && safeOwner) {
                                 refreshMonitorTargetCombo(safeCombo, safeOwner);
                             }
                         });
        QObject::connect(qGuiApp, &QGuiApplication::screenRemoved, combo,
                         [safeCombo, safeOwner](QScreen *) {
                             if (safeCombo && safeOwner) {
                                 refreshMonitorTargetCombo(safeCombo, safeOwner);
                             }
                         });
    }

    // Re-check once MainWindow has finished constructing sibling docks/topology bars.
    QPointer<QComboBox> safeCombo(combo);
    QPointer<QWidget> safeOwner(ownerWidget);
    QTimer::singleShot(0, combo, [safeCombo, safeOwner]() {
        if (safeCombo && safeOwner) {
            refreshMonitorTargetCombo(safeCombo, safeOwner);
        }
    });
}

} // namespace wwal::studio::browser_shared
