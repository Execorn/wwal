// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "waywal/palette_hct.h"

#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

namespace waywal::studio {

enum class TemplateTargetId {
    Hyprland = 0,
    Waybar,
    Caelestia,
    Kitty,
    Alacritty,
    RofiFuzzel,
    Mako,
    Gtk3Gtk4,
    Count
};

struct TemplateTargetDescriptor {
    TemplateTargetId id;
    QString key;
    QString displayName;
    QString primaryPath;
    QString secondaryPath; // Optional second file (e.g. Fuzzel for Rofi, or GTK4 for GTK3)
    QString description;
    bool defaultEnabled{true};
};

/**
 * @brief Manages exporting Material You HCT color tokens into desktop application
 *        configuration files with atomic write-temp-and-rename semantics.
 */
class TemplateExporter : public QWidget {
    Q_OBJECT

public:
    explicit TemplateExporter(QWidget *parent = nullptr);
    ~TemplateExporter() override = default;

    void setPalette(const waywal_palette_t &palette);
    [[nodiscard]] const waywal_palette_t &palette() const { return m_palette; }
    [[nodiscard]] bool hasPalette() const { return m_hasPalette; }

    // Static formatters
    static QString formatHyprland(const waywal_palette_t &pal);
    static QString formatWaybar(const waywal_palette_t &pal);
    static QString formatCaelestia(const waywal_palette_t &pal);
    static QString formatKitty(const waywal_palette_t &pal);
    static QString formatAlacritty(const waywal_palette_t &pal);
    static QString formatRofi(const waywal_palette_t &pal);
    static QString formatFuzzel(const waywal_palette_t &pal);
    static QString formatMako(const waywal_palette_t &pal);
    static QString formatGtkCss(const waywal_palette_t &pal);

    // Target metadata
    static const QVector<TemplateTargetDescriptor> &targetDescriptors();

    // Export operations
    static QString resolveDestination(const QString &relPath, const QString &overrideBaseDir = QString());
    static bool atomicWrite(const QString &targetPath, const QString &content, QString *errorOut = nullptr);

    bool exportTarget(TemplateTargetId id, const QString &overrideBaseDir = QString(), QString *errorOut = nullptr);
    int exportSelected(const QString &overrideBaseDir = QString(), QStringList *errorsOut = nullptr);
    int exportAll(const QString &overrideBaseDir = QString(), QStringList *errorsOut = nullptr);

    [[nodiscard]] bool isTargetEnabled(TemplateTargetId id) const;
    void setTargetEnabled(TemplateTargetId id, bool enabled);

Q_SIGNALS:
    void exportCompleted(int successCount, int totalCount, const QString &statusMessage);

public Q_SLOTS:
    void exportSelectedNow();
    void selectAll();
    void deselectAll();

private:
    void setupUi();

    static QString hexRgb(const color_rgba_t &c);
    static QString hexRgba(const color_rgba_t &c);
    static QString hyprRgb(const color_rgba_t &c);

    waywal_palette_t m_palette{};
    bool m_hasPalette{false};

    struct TargetItem {
        TemplateTargetDescriptor desc;
        QCheckBox *checkBox{nullptr};
        QLabel *pathLabel{nullptr};
    };

    QVector<TargetItem> m_targets;
    QPushButton *m_exportBtn{nullptr};
    QPushButton *m_selectAllBtn{nullptr};
    QPushButton *m_deselectAllBtn{nullptr};
    QLabel *m_statusLabel{nullptr};
};

} // namespace waywal::studio
