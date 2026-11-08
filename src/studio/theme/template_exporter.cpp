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

#include <cstdio>
#include <unistd.h>

namespace waywal::studio {

const QVector<TemplateTargetDescriptor> &TemplateExporter::targetDescriptors()
{
    static const QVector<TemplateTargetDescriptor> descriptors = {
        {TemplateTargetId::Hyprland, QStringLiteral("hyprland"), QStringLiteral("Hyprland"),
         QStringLiteral("hypr/colors.conf"), QString(),
         QStringLiteral("Active border 45° gradient, inactive borders, and color variables"), true},
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
    mainLayout->setContentsMargins(6, 6, 6, 6);
    mainLayout->setSpacing(6);

    auto *headerLabel = new QLabel(QStringLiteral("<b>Desktop Template Exporter</b>"), this);
    headerLabel->setStyleSheet(QStringLiteral("font-size: 10pt;"));
    mainLayout->addWidget(headerLabel);

    auto *descLabel = new QLabel(QStringLiteral("Export Material You HCT color tokens atomically "
                                                "to standard $XDG_CONFIG_HOME target templates."),
                                 this);
    descLabel->setStyleSheet(QStringLiteral("color: #888888; font-size: 8pt;"));
    descLabel->setWordWrap(true);
    mainLayout->addWidget(descLabel);

    // Target Checkbox List
    auto *targetsLayout = new QVBoxLayout();
    targetsLayout->setSpacing(4);

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
            QStringLiteral("color: #777777; font-family: monospace; font-size: 8pt;"));
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
    btnLayout->setSpacing(6);

    m_selectAllBtn = new QPushButton(QStringLiteral("Select All"), this);
    connect(m_selectAllBtn, &QPushButton::clicked, this, &TemplateExporter::selectAll);
    btnLayout->addWidget(m_selectAllBtn);

    m_deselectAllBtn = new QPushButton(QStringLiteral("Deselect All"), this);
    connect(m_deselectAllBtn, &QPushButton::clicked, this, &TemplateExporter::deselectAll);
    btnLayout->addWidget(m_deselectAllBtn);

    btnLayout->addStretch(1);
    mainLayout->addLayout(btnLayout);

    m_exportBtn = new QPushButton(QStringLiteral("Export Selected to ~/.config"), this);
    m_exportBtn->setStyleSheet(
        QStringLiteral("QPushButton { background-color: #6750A4; color: white; font-weight: bold; "
                       "padding: 6px 12px; border-radius: 6px; } "
                       "QPushButton:hover { background-color: #7D64B8; } "
                       "QPushButton:pressed { background-color: #533C8F; }"));
    connect(m_exportBtn, &QPushButton::clicked, this, &TemplateExporter::exportSelectedNow);
    mainLayout->addWidget(m_exportBtn);

    // Status Label
    m_statusLabel = new QLabel(QStringLiteral("Ready to export."), this);
    m_statusLabel->setStyleSheet(QStringLiteral("color: #888888; font-size: 8.5pt;"));
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
    return QString::asprintf("#%02x%02x%02x", c.r, c.g, c.b);
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
    if (!overrideBaseDir.isEmpty()) {
        return QDir(overrideBaseDir).filePath(relPath);
    }

    QString configHome = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (configHome.isEmpty()) {
        configHome = QDir::homePath() + QStringLiteral("/.config");
    }
    return QDir(configHome).filePath(relPath);
}

bool TemplateExporter::atomicWrite(const QString &targetPath, const QString &content,
                                   QString *errorOut)
{
    QFileInfo targetInfo(targetPath);
    QDir parentDir = targetInfo.dir();

    if (!parentDir.exists()) {
        if (!parentDir.mkpath(QStringLiteral("."))) {
            if (errorOut) {
                *errorOut = QStringLiteral("Failed to create parent directory: ") +
                            parentDir.absolutePath();
            }
            return false;
        }
    }

    const quint32 randSuffix = QRandomGenerator::global()->generate();
    const QString tempPath =
        targetPath +
        QStringLiteral(".tmp.%1.%2").arg(QCoreApplication::applicationPid()).arg(randSuffix);

    QFile tempFile(tempPath);
    if (!tempFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Cannot open temporary file for write: ") + tempPath;
        }
        return false;
    }

    const QByteArray utf8 = content.toUtf8();
    if (tempFile.write(utf8) != utf8.size()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Incomplete write to temporary file: ") + tempPath;
        }
        tempFile.close();
        QFile::remove(tempPath);
        return false;
    }

    tempFile.flush();
#if defined(Q_OS_UNIX)
    const int fd = tempFile.handle();
    if (fd >= 0) {
        fsync(fd);
    }
#endif
    tempFile.close();

    // Atomic rename using POSIX rename() to replace existing file atomically
    const QByteArray oldLocal = tempPath.toLocal8Bit();
    const QByteArray newLocal = targetPath.toLocal8Bit();

    if (::rename(oldLocal.constData(), newLocal.constData()) != 0) {
        // Fallback to QFile::rename
        QFile::remove(targetPath);
        if (!QFile::rename(tempPath, targetPath)) {
            if (errorOut) {
                *errorOut = QStringLiteral("Atomic rename failed from ") + tempPath +
                            QStringLiteral(" to ") + targetPath;
            }
            QFile::remove(tempPath);
            return false;
        }
    }

    return true;
}

QString TemplateExporter::formatHyprland(const waywal_palette_t &pal)
{
    QString out;
    out += QStringLiteral("# Generated by wwal-studio Material You HCT Live Theme Studio\n");
    out += QStringLiteral("# Auto-generated on ") +
           QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("\n\n");

    out += QStringLiteral("$primary = ") + hyprRgb(pal.primary) + QStringLiteral("\n");
    out += QStringLiteral("$on_primary = ") + hyprRgb(pal.onPrimary) + QStringLiteral("\n");
    out += QStringLiteral("$primary_container = ") + hyprRgb(pal.primaryContainer) +
           QStringLiteral("\n");
    out += QStringLiteral("$on_primary_container = ") + hyprRgb(pal.onPrimaryContainer) +
           QStringLiteral("\n");
    out += QStringLiteral("$inverse_primary = ") + hyprRgb(pal.inversePrimary) +
           QStringLiteral("\n\n");

    out += QStringLiteral("$secondary = ") + hyprRgb(pal.secondary) + QStringLiteral("\n");
    out += QStringLiteral("$on_secondary = ") + hyprRgb(pal.onSecondary) + QStringLiteral("\n");
    out += QStringLiteral("$secondary_container = ") + hyprRgb(pal.secondaryContainer) +
           QStringLiteral("\n");
    out += QStringLiteral("$on_secondary_container = ") + hyprRgb(pal.onSecondaryContainer) +
           QStringLiteral("\n\n");

    out += QStringLiteral("$tertiary = ") + hyprRgb(pal.tertiary) + QStringLiteral("\n");
    out += QStringLiteral("$on_tertiary = ") + hyprRgb(pal.onTertiary) + QStringLiteral("\n");
    out += QStringLiteral("$tertiary_container = ") + hyprRgb(pal.tertiaryContainer) +
           QStringLiteral("\n");
    out += QStringLiteral("$on_tertiary_container = ") + hyprRgb(pal.onTertiaryContainer) +
           QStringLiteral("\n\n");

    out += QStringLiteral("$error = ") + hyprRgb(pal.error) + QStringLiteral("\n");
    out += QStringLiteral("$on_error = ") + hyprRgb(pal.onError) + QStringLiteral("\n");
    out +=
        QStringLiteral("$error_container = ") + hyprRgb(pal.errorContainer) + QStringLiteral("\n");
    out += QStringLiteral("$on_error_container = ") + hyprRgb(pal.onErrorContainer) +
           QStringLiteral("\n\n");

    out += QStringLiteral("$background = ") + hyprRgb(pal.background) + QStringLiteral("\n");
    out += QStringLiteral("$on_background = ") + hyprRgb(pal.onBackground) + QStringLiteral("\n");
    out += QStringLiteral("$surface = ") + hyprRgb(pal.surface) + QStringLiteral("\n");
    out += QStringLiteral("$on_surface = ") + hyprRgb(pal.onSurface) + QStringLiteral("\n");
    out += QStringLiteral("$surface_dim = ") + hyprRgb(pal.surfaceDim) + QStringLiteral("\n");
    out += QStringLiteral("$surface_bright = ") + hyprRgb(pal.surfaceBright) + QStringLiteral("\n");
    out +=
        QStringLiteral("$surface_variant = ") + hyprRgb(pal.surfaceVariant) + QStringLiteral("\n");
    out += QStringLiteral("$on_surface_variant = ") + hyprRgb(pal.onSurfaceVariant) +
           QStringLiteral("\n");
    out += QStringLiteral("$surface_container_lowest = ") + hyprRgb(pal.surfaceContainerLowest) +
           QStringLiteral("\n");
    out += QStringLiteral("$surface_container_low = ") + hyprRgb(pal.surfaceContainerLow) +
           QStringLiteral("\n");
    out += QStringLiteral("$surface_container = ") + hyprRgb(pal.surfaceContainer) +
           QStringLiteral("\n");
    out += QStringLiteral("$surface_container_high = ") + hyprRgb(pal.surfaceContainerHigh) +
           QStringLiteral("\n");
    out += QStringLiteral("$surface_container_highest = ") + hyprRgb(pal.surfaceContainerHighest) +
           QStringLiteral("\n\n");

    out += QStringLiteral("$outline = ") + hyprRgb(pal.outline) + QStringLiteral("\n");
    out +=
        QStringLiteral("$outline_variant = ") + hyprRgb(pal.outlineVariant) + QStringLiteral("\n");
    out += QStringLiteral("$shadow = ") + hyprRgb(pal.shadow) + QStringLiteral("\n\n");

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

    const auto addCss = [&](const char *name, const color_rgba_t &c) {
        out += QString::asprintf("@define-color %s #%02x%02x%02x;\n", name, c.r, c.g, c.b);
    };

    addCss("primary", pal.primary);
    addCss("on_primary", pal.onPrimary);
    addCss("primary_container", pal.primaryContainer);
    addCss("on_primary_container", pal.onPrimaryContainer);
    addCss("inverse_primary", pal.inversePrimary);

    addCss("secondary", pal.secondary);
    addCss("on_secondary", pal.onSecondary);
    addCss("secondary_container", pal.secondaryContainer);
    addCss("on_secondary_container", pal.onSecondaryContainer);

    addCss("tertiary", pal.tertiary);
    addCss("on_tertiary", pal.onTertiary);
    addCss("tertiary_container", pal.tertiaryContainer);
    addCss("on_tertiary_container", pal.onTertiaryContainer);

    addCss("error", pal.error);
    addCss("on_error", pal.onError);
    addCss("error_container", pal.errorContainer);
    addCss("on_error_container", pal.onErrorContainer);

    addCss("background", pal.background);
    addCss("on_background", pal.onBackground);
    addCss("surface", pal.surface);
    addCss("on_surface", pal.onSurface);
    addCss("surface_dim", pal.surfaceDim);
    addCss("surface_bright", pal.surfaceBright);
    addCss("surface_variant", pal.surfaceVariant);
    addCss("on_surface_variant", pal.onSurfaceVariant);

    addCss("surface_container_lowest", pal.surfaceContainerLowest);
    addCss("surface_container_low", pal.surfaceContainerLow);
    addCss("surface_container", pal.surfaceContainer);
    addCss("surface_container_high", pal.surfaceContainerHigh);
    addCss("surface_container_highest", pal.surfaceContainerHighest);

    addCss("outline", pal.outline);
    addCss("outline_variant", pal.outlineVariant);
    addCss("shadow", pal.shadow);

    return out;
}

QString TemplateExporter::formatCaelestia(const waywal_palette_t &pal)
{
    QJsonObject root;
    root[QStringLiteral("generator")] = QStringLiteral("wwal-studio");
    root[QStringLiteral("timestamp")] = QDateTime::currentDateTime().toString(Qt::ISODate);
    root[QStringLiteral("mode")] =
        (pal.mode == WAYWAL_THEME_LIGHT) ? QStringLiteral("light") : QStringLiteral("dark");
    root[QStringLiteral("contrast")] = static_cast<double>(pal.contrast);

    QJsonObject colors;
    colors[QStringLiteral("primary")] = hexRgb(pal.primary);
    colors[QStringLiteral("onPrimary")] = hexRgb(pal.onPrimary);
    colors[QStringLiteral("primaryContainer")] = hexRgb(pal.primaryContainer);
    colors[QStringLiteral("onPrimaryContainer")] = hexRgb(pal.onPrimaryContainer);
    colors[QStringLiteral("inversePrimary")] = hexRgb(pal.inversePrimary);

    colors[QStringLiteral("secondary")] = hexRgb(pal.secondary);
    colors[QStringLiteral("onSecondary")] = hexRgb(pal.onSecondary);
    colors[QStringLiteral("secondaryContainer")] = hexRgb(pal.secondaryContainer);
    colors[QStringLiteral("onSecondaryContainer")] = hexRgb(pal.onSecondaryContainer);

    colors[QStringLiteral("tertiary")] = hexRgb(pal.tertiary);
    colors[QStringLiteral("onTertiary")] = hexRgb(pal.onTertiary);
    colors[QStringLiteral("tertiaryContainer")] = hexRgb(pal.tertiaryContainer);
    colors[QStringLiteral("onTertiaryContainer")] = hexRgb(pal.onTertiaryContainer);

    colors[QStringLiteral("error")] = hexRgb(pal.error);
    colors[QStringLiteral("onError")] = hexRgb(pal.onError);
    colors[QStringLiteral("errorContainer")] = hexRgb(pal.errorContainer);
    colors[QStringLiteral("onErrorContainer")] = hexRgb(pal.onErrorContainer);

    colors[QStringLiteral("background")] = hexRgb(pal.background);
    colors[QStringLiteral("onBackground")] = hexRgb(pal.onBackground);
    colors[QStringLiteral("surface")] = hexRgb(pal.surface);
    colors[QStringLiteral("onSurface")] = hexRgb(pal.onSurface);
    colors[QStringLiteral("surfaceDim")] = hexRgb(pal.surfaceDim);
    colors[QStringLiteral("surfaceBright")] = hexRgb(pal.surfaceBright);
    colors[QStringLiteral("surfaceVariant")] = hexRgb(pal.surfaceVariant);
    colors[QStringLiteral("onSurfaceVariant")] = hexRgb(pal.onSurfaceVariant);

    colors[QStringLiteral("surfaceContainerLowest")] = hexRgb(pal.surfaceContainerLowest);
    colors[QStringLiteral("surfaceContainerLow")] = hexRgb(pal.surfaceContainerLow);
    colors[QStringLiteral("surfaceContainer")] = hexRgb(pal.surfaceContainer);
    colors[QStringLiteral("surfaceContainerHigh")] = hexRgb(pal.surfaceContainerHigh);
    colors[QStringLiteral("surfaceContainerHighest")] = hexRgb(pal.surfaceContainerHighest);

    colors[QStringLiteral("outline")] = hexRgb(pal.outline);
    colors[QStringLiteral("outlineVariant")] = hexRgb(pal.outlineVariant);
    colors[QStringLiteral("shadow")] = hexRgb(pal.shadow);
    root[QStringLiteral("colors")] = colors;

    // ANSI Terminals
    QJsonArray termArray;
    for (int i = 0; i < 16; ++i) {
        termArray.append(hexRgb(pal.term[i]));
    }
    root[QStringLiteral("term")] = termArray;

    // Catppuccin tokens
    QJsonObject catppuccin;
    catppuccin[QStringLiteral("rosewater")] = hexRgb(pal.rosewater);
    catppuccin[QStringLiteral("flamingo")] = hexRgb(pal.flamingo);
    catppuccin[QStringLiteral("pink")] = hexRgb(pal.pink);
    catppuccin[QStringLiteral("mauve")] = hexRgb(pal.mauve);
    catppuccin[QStringLiteral("red")] = hexRgb(pal.red);
    catppuccin[QStringLiteral("maroon")] = hexRgb(pal.maroon);
    catppuccin[QStringLiteral("peach")] = hexRgb(pal.peach);
    catppuccin[QStringLiteral("yellow")] = hexRgb(pal.yellow);
    catppuccin[QStringLiteral("green")] = hexRgb(pal.green);
    catppuccin[QStringLiteral("teal")] = hexRgb(pal.teal);
    catppuccin[QStringLiteral("sky")] = hexRgb(pal.sky);
    catppuccin[QStringLiteral("sapphire")] = hexRgb(pal.sapphire);
    catppuccin[QStringLiteral("blue")] = hexRgb(pal.blue);
    catppuccin[QStringLiteral("lavender")] = hexRgb(pal.lavender);
    root[QStringLiteral("catppuccin")] = catppuccin;

    QJsonDocument doc(root);
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
        out += QString::asprintf("color%d #%02x%02x%02x\n", i, pal.term[i].r, pal.term[i].g,
                                 pal.term[i].b);
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

    out += QStringLiteral("[colors]\n");
    out += QString::asprintf("background=%02x%02x%02xfa\n", pal.surfaceContainerLowest.r,
                             pal.surfaceContainerLowest.g, pal.surfaceContainerLowest.b);
    out += QString::asprintf("text=%02x%02x%02xff\n", pal.onSurface.r, pal.onSurface.g,
                             pal.onSurface.b);
    out += QString::asprintf("match=%02x%02x%02xff\n", pal.primary.r, pal.primary.g, pal.primary.b);
    out += QString::asprintf("selection=%02x%02x%02xff\n", pal.primaryContainer.r,
                             pal.primaryContainer.g, pal.primaryContainer.b);
    out += QString::asprintf("selection-text=%02x%02x%02xff\n", pal.onPrimaryContainer.r,
                             pal.onPrimaryContainer.g, pal.onPrimaryContainer.b);
    out += QString::asprintf("selection-match=%02x%02x%02xff\n", pal.tertiary.r, pal.tertiary.g,
                             pal.tertiary.b);
    out +=
        QString::asprintf("border=%02x%02x%02xff\n", pal.primary.r, pal.primary.g, pal.primary.b);

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
        out += QString::asprintf("@define-color %s #%02x%02x%02x;\n", name, c.r, c.g, c.b);
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

    switch (id) {
    case TemplateTargetId::Hyprland: {
        const QString path =
            resolveDestination(QStringLiteral("hypr/colors.conf"), overrideBaseDir);
        return atomicWrite(path, formatHyprland(m_palette), errorOut);
    }
    case TemplateTargetId::Waybar: {
        const QString path =
            resolveDestination(QStringLiteral("waybar/colors.css"), overrideBaseDir);
        return atomicWrite(path, formatWaybar(m_palette), errorOut);
    }
    case TemplateTargetId::Caelestia: {
        const QString path =
            resolveDestination(QStringLiteral("quickshell/caelestia/colors.json"), overrideBaseDir);
        return atomicWrite(path, formatCaelestia(m_palette), errorOut);
    }
    case TemplateTargetId::Kitty: {
        const QString path =
            resolveDestination(QStringLiteral("kitty/kitty-colors.conf"), overrideBaseDir);
        return atomicWrite(path, formatKitty(m_palette), errorOut);
    }
    case TemplateTargetId::Alacritty: {
        const QString path =
            resolveDestination(QStringLiteral("alacritty/colors.toml"), overrideBaseDir);
        return atomicWrite(path, formatAlacritty(m_palette), errorOut);
    }
    case TemplateTargetId::RofiFuzzel: {
        const QString rofiPath =
            resolveDestination(QStringLiteral("rofi/colors.rasi"), overrideBaseDir);
        const QString fuzzelPath =
            resolveDestination(QStringLiteral("fuzzel/colors.ini"), overrideBaseDir);
        bool ok = atomicWrite(rofiPath, formatRofi(m_palette), errorOut);
        if (ok) {
            ok = atomicWrite(fuzzelPath, formatFuzzel(m_palette), errorOut);
        }
        return ok;
    }
    case TemplateTargetId::Mako: {
        const QString path =
            resolveDestination(QStringLiteral("mako/config-colors"), overrideBaseDir);
        return atomicWrite(path, formatMako(m_palette), errorOut);
    }
    case TemplateTargetId::Gtk3Gtk4: {
        const QString gtk3Path =
            resolveDestination(QStringLiteral("gtk-3.0/waywal-colors.css"), overrideBaseDir);
        const QString gtk4Path =
            resolveDestination(QStringLiteral("gtk-4.0/waywal-colors.css"), overrideBaseDir);
        const QString content = formatGtkCss(m_palette);
        bool ok = atomicWrite(gtk3Path, content, errorOut);
        if (ok) {
            ok = atomicWrite(gtk4Path, content, errorOut);
        }
        return ok;
    }
    default:
        break;
    }

    if (errorOut) {
        *errorOut = QStringLiteral("Unknown export target ID.");
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
