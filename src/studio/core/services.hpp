// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_SERVICES_HPP
#define WAYWAL_STUDIO_SERVICES_HPP

#include "waywal/ipc_proto.h"
#include "waywal/bezier.h"

#include <QString>
#include <QStringList>
#include <QVector>
#include <QColor>
#include <QJsonObject>
#include <QObject>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace waywal::studio {

/* ========================================================================= */
/* Shared Data Structures                                                    */
/* ========================================================================= */

struct UniformValue {
    enum class Type : uint8_t {
        Float,
        Int,
        Bool,
        Vec2,
        Vec3,
        Vec4,
        Color,
        String
    };

    Type type{Type::Float};
    float floats[4]{0.0f, 0.0f, 0.0f, 0.0f};
    int32_t intVal{0};
    bool boolVal{false};
    QString stringVal;

    UniformValue() = default;

    static UniformValue fromFloat(float val) {
        UniformValue v;
        v.type = Type::Float;
        v.floats[0] = val;
        return v;
    }

    static UniformValue fromInt(int32_t val) {
        UniformValue v;
        v.type = Type::Int;
        v.intVal = val;
        v.floats[0] = static_cast<float>(val);
        return v;
    }

    static UniformValue fromBool(bool val) {
        UniformValue v;
        v.type = Type::Bool;
        v.boolVal = val;
        v.floats[0] = val ? 1.0f : 0.0f;
        return v;
    }

    static UniformValue fromVec2(float x, float y) {
        UniformValue v;
        v.type = Type::Vec2;
        v.floats[0] = x;
        v.floats[1] = y;
        return v;
    }

    static UniformValue fromVec3(float x, float y, float z) {
        UniformValue v;
        v.type = Type::Vec3;
        v.floats[0] = x;
        v.floats[1] = y;
        v.floats[2] = z;
        return v;
    }

    static UniformValue fromVec4(float x, float y, float z, float w) {
        UniformValue v;
        v.type = Type::Vec4;
        v.floats[0] = x;
        v.floats[1] = y;
        v.floats[2] = z;
        v.floats[3] = w;
        return v;
    }

    static UniformValue fromColor(const QColor &col) {
        UniformValue v;
        v.type = Type::Color;
        v.floats[0] = static_cast<float>(col.redF());
        v.floats[1] = static_cast<float>(col.greenF());
        v.floats[2] = static_cast<float>(col.blueF());
        v.floats[3] = static_cast<float>(col.alphaF());
        v.stringVal = col.name(QColor::HexRgb);
        return v;
    }

    static UniformValue fromString(const QString &str) {
        UniformValue v;
        v.type = Type::String;
        v.stringVal = str;
        return v;
    }

    uint32_t getFloatCount() const {
        switch (type) {
        case Type::Float:
        case Type::Int:
        case Type::Bool:
            return 1;
        case Type::Vec2:
            return 2;
        case Type::Vec3:
            return 3;
        case Type::Vec4:
        case Type::Color:
            return 4;
        case Type::String:
            return 0;
        }
        return 1;
    }

    bool toFloats(float *out, uint32_t &outCount) const {
        if (!out) return false;
        outCount = getFloatCount();
        for (uint32_t i = 0; i < outCount; ++i) {
            out[i] = floats[i];
        }
        return true;
    }

    bool operator==(const UniformValue &other) const {
        if (type != other.type) return false;
        switch (type) {
        case Type::Float:
        case Type::Vec2:
        case Type::Vec3:
        case Type::Vec4:
        case Type::Color:
            for (uint32_t i = 0; i < getFloatCount(); ++i) {
                if (floats[i] != other.floats[i]) return false;
            }
            return true;
        case Type::Int:
            return intVal == other.intVal;
        case Type::Bool:
            return boolVal == other.boolVal;
        case Type::String:
            return stringVal == other.stringVal;
        }
        return false;
    }
};

struct OutputTopologyInfo {
    QString name;             // e.g. "DP-1"
    QString description;      // Monitor make/model
    int32_t x{0};             // Compositor global X
    int32_t y{0};             // Compositor global Y
    uint32_t width{0};        // Pixel resolution
    uint32_t height{0};
    uint32_t logicalWidth{0}; // Scaled resolution
    uint32_t logicalHeight{0};
    double refreshRateHz{60.0};
    double scale{1.0};
    bool isPrimary{false};
    bool isEnabled{true};
    QString currentWallpaperPath;
    QString currentWallpaperType; // "image", "video", "shader", etc.
};

struct OutputTelemetry {
    QString name;
    float scanoutFps{0.0f};
    uint32_t lastRenderUs{0};
    uint32_t vramKiB{0};
    uint8_t boStates[4]{0, 0, 0, 0}; // 0=Free, 1=Rendering, 2=Committed, 3=Locked
    uint8_t engineKind{0};           // 0=static, 1=shader, 2=workshop, 3=video, 4=slideshow, 5=animation
    bool isPaused{false};
    uint16_t pauseMask{0};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t logicalWidth{0};
    uint32_t logicalHeight{0};
    double scale{1.0};
};

struct TelemetrySnapshot {
    uint64_t timestampNs{0};
    uint32_t rssKiB{0};
    uint32_t pssKiB{0};
    uint32_t totalVramKiB{0};
    bool gpuActive{true};

    bool onBattery{false};
    uint8_t batteryPolicy{0}; // 0=ignore, 1=reduce-fps, 2=pause-video
    uint16_t globalPauseMask{0};

    std::vector<float> barkBands; // 64 Bark-scale frequency bands
    float rmsLevel{0.0f};
    float beatPulse{0.0f};
    bool beatOnset{false};
    bool audioActive{false};
    bool pipewireActive{false};
    bool muteOnAudio{false};
    float audioVolume{1.0f};

    std::vector<OutputTelemetry> outputs;
};

enum class WallpaperSourceKind : uint8_t {
    Image = 0,
    Video = 1,
    Shader = 2,
    WorkshopScene = 3,
    GnomeXml = 4,
    SolidColor = 5
};

struct WallpaperCatalogEntry {
    QString id;                    // Unique identifier (hash or path-derived)
    QString title;                 // Human-readable title
    QString description;
    QString author;
    QString filePath;              // Full absolute path
    QString previewThumbnailPath;  // Cached WebP thumbnail path
    WallpaperSourceKind sourceKind{WallpaperSourceKind::Image};
    uint64_t tagBitmask{0};
    uint64_t fileSizeBytes{0};
    int64_t lastModifiedSec{0};
    uint32_t width{0};
    uint32_t height{0};
    double aspectRatio{16.0 / 9.0};
    uint32_t dominantColorRgba{0xFF808080};
    bool isFavorite{false};
};

/* ========================================================================= */
/* Decoupled Service Interfaces                                              */
/* ========================================================================= */

class IIpcClientService {
public:
    virtual ~IIpcClientService() = default;

    virtual bool isConnected() const = 0;
    virtual void sendSetUniform(const QString &output, const QString &name,
                                const float *vals, uint32_t count, bool persist = false) = 0;
    virtual void sendSetImage(const waywal_img_metadata_t &meta, int memfd = -1) = 0;
    virtual void sendAudioPowerConfig(float volume, int8_t muteOnAudio,
                                      int8_t themeFromMedia, uint8_t batteryPolicy = 0) = 0;
    virtual void requestOutputsJson(std::function<void(const QString &json)> callback = nullptr) = 0;
    virtual void requestTelemetry(std::function<void(const TelemetrySnapshot &snapshot)> callback = nullptr) = 0;
    virtual void requestOutputDmabuf(const QString &output,
                                     std::function<void(const waywal_export_dmabuf_resp_t &desc,
                                                        const QVector<int> &fds)> callback = nullptr) = 0;
    virtual void subscribeEvents() = 0;
};

class IVirtualizedGridService {
public:
    virtual ~IVirtualizedGridService() = default;

    virtual size_t entryCount() const = 0;
    virtual const WallpaperCatalogEntry *entryAt(size_t index) const = 0;
    virtual void refreshCatalog() = 0;
    virtual void setFilterQuery(const QString &query, uint64_t tagMask = 0) = 0;
};

class IThemeEngineService {
public:
    virtual ~IThemeEngineService() = default;

    virtual bool extractThemeFromImage(const QString &imagePath, bool lightMode = false) = 0;
    virtual QColor seedColor() const = 0;
    virtual QVector<QColor> tonalPalette(const QString &role) const = 0;
    virtual bool exportThemesToDesktop() = 0;
};

class IAudioTelemetryService {
public:
    virtual ~IAudioTelemetryService() = default;

    virtual TelemetrySnapshot currentTelemetry() const = 0;
    virtual float masterGain() const = 0;
    virtual void setMasterGain(float gain) = 0;
    virtual bool muteOnExternalAudio() const = 0;
    virtual void setMuteOnExternalAudio(bool enabled) = 0;
};

/* ========================================================================= */
/* Service Locator                                                           */
/* ========================================================================= */

class Services {
public:
    static Services &instance() {
        static Services s_instance;
        return s_instance;
    }

    void setIpcClient(IIpcClientService *svc) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_ipcClient = svc;
    }

    IIpcClientService *ipcClient() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_ipcClient;
    }

    void setGridService(IVirtualizedGridService *svc) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_gridService = svc;
    }

    IVirtualizedGridService *gridService() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_gridService;
    }

    void setThemeEngine(IThemeEngineService *svc) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_themeEngine = svc;
    }

    IThemeEngineService *themeEngine() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_themeEngine;
    }

    void setAudioTelemetry(IAudioTelemetryService *svc) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_audioTelemetry = svc;
    }

    IAudioTelemetryService *audioTelemetry() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_audioTelemetry;
    }

private:
    Services() = default;
    ~Services() = default;
    Services(const Services &) = delete;
    Services &operator=(const Services &) = delete;

    mutable std::mutex m_mutex;
    IIpcClientService *m_ipcClient{nullptr};
    IVirtualizedGridService *m_gridService{nullptr};
    IThemeEngineService *m_themeEngine{nullptr};
    IAudioTelemetryService *m_audioTelemetry{nullptr};
};

} // namespace waywal::studio

#endif // WAYWAL_STUDIO_SERVICES_HPP
