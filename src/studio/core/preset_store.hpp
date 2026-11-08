// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_PRESET_STORE_HPP
#define WAYWAL_STUDIO_PRESET_STORE_HPP

#include "services.hpp"
#include "waywal/bezier.h"
#include "waywal/path.h"
#include "waywal/toml_parser.h"

#include <QDir>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>

#include <map>

namespace waywal::studio {

/* ========================================================================= */
/* Wallpaper Preset Data Model                                               */
/* ========================================================================= */

struct WallpaperPreset {
    QString id;
    QString wallpaperPath;
    QString title;

    // Transition parameters
    QString transitionType{QStringLiteral("fade")};
    uint32_t transitionDurationMs{1000};
    uint32_t transitionFps{60};
    bezier_curve_t bezier{0.25f, 0.1f, 0.25f, 1.0f};
    bool hasCustomBezier{false};

    // Power & Audio parameters
    QString batteryPolicy{QStringLiteral("ignore")};
    bool muteOnAudio{false};
    float volume{1.0f};

    // Layout parameters
    float cropGravityX{0.5f};
    float cropGravityY{0.5f};
    bool hasCropGravity{false};

    // Uniform & property overrides (e.g. u_speed, bloom, schemecolor)
    std::map<QString, UniformValue> uniforms;
    std::map<QString, QString> extraProperties;

    bool operator==(const WallpaperPreset &other) const = default;
};

/* ========================================================================= */
/* Preset Store Management (~/.config/waywal/presets/<id>.toml)              */
/* ========================================================================= */

class PresetStore : public QObject {
    Q_OBJECT

public:
    explicit PresetStore(QString baseDir = QString(), QObject *parent = nullptr);
    ~PresetStore() override = default;

    [[nodiscard]] QString presetDirectory() const;
    [[nodiscard]] QString resolvePresetFilePath(const QString &id) const;

    bool savePreset(const WallpaperPreset &preset);
    bool loadPreset(const QString &id, WallpaperPreset &outPreset);
    bool deletePreset(const QString &id);
    [[nodiscard]] bool hasPreset(const QString &id) const;
    [[nodiscard]] QStringList listPresetIds() const;
    [[nodiscard]] QList<WallpaperPreset> listAllPresets() const;

    static QString sanitizeId(const QString &id);

signals:
    void presetSaved(const QString &id);
    void presetDeleted(const QString &id);
    void errorOccurred(const QString &error);

private:
    void ensureDirectoryExists() const;
    QString m_baseDir;
};

} // namespace waywal::studio

#endif // WAYWAL_STUDIO_PRESET_STORE_HPP
