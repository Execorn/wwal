// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_INSPECTOR_PROPERTY_INSPECTOR_WIDGET_HPP
#define WAYWAL_STUDIO_INSPECTOR_PROPERTY_INSPECTOR_WIDGET_HPP

#include "oklch_color_dialog.hpp"
#include "studio/core/command_bus.hpp"
#include "studio/core/ipc_client.hpp"
#include "waywal/shader_parser.h"
#include "waywal/we_project.h"

#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QString>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>

#include <array>
#include <memory>
#include <vector>

namespace waywal::studio {

class PresetStore; // Forward declaration for preset store service

/**
 * @brief Unified property representation for both Wallpaper Engine
 * user properties and Shadertoy GLSL uniform pragmas.
 */
struct InspectorProperty {
    enum class Kind {
        Float,
        Vec2,
        Vec3,
        Vec4,
        Bool,
        Int,
        Color,
        Text,
        Combo
    };

    QString id;
    QString name;
    QString label;
    QString category;
    Kind kind{Kind::Float};
    uint32_t numComponents{1};

    std::vector<float> currentValues{0.0f};
    std::vector<float> defaultValues{0.0f};
    std::vector<float> minValues{0.0f};
    std::vector<float> maxValues{1.0f};
    float step{0.01f};
    bool hasRange{false};

    QString textValue;
    QString defaultTextValue;
    QStringList comboOptions;
    int comboIndex{0};
};

/**
 * @brief UI control row for an individual property in the inspector.
 */
class PropertyRowWidget : public QWidget {
    Q_OBJECT

public:
    explicit PropertyRowWidget(const InspectorProperty &prop, QWidget *parent = nullptr);
    ~PropertyRowWidget() override = default;

    [[nodiscard]] const InspectorProperty &property() const noexcept { return m_prop; }
    void updateValues(const std::vector<float> &vals);
    void resetToDefault();
    void setFilterVisible(bool visible);

signals:
    void valueChanged(const QString &name, const std::vector<float> &values, uint32_t count);

private slots:
    void onSliderValueChanged(int index, int sliderVal);
    void onSpinValueChanged(int index, double spinVal);
    void onBoolToggled(bool checked);
    void onIntSpinChanged(int val);
    void onComboIndexChanged(int idx);
    void onColorButtonClicked();
    void onHexEdited(const QString &text);
    void onTextEdited(const QString &text);

private:
    void setupUi();
    void syncSliderSpin(int index, float val);
    void emitChange();

    InspectorProperty m_prop;
    bool m_updating{false};

    QLabel *m_labelWidget{nullptr};
    QToolButton *m_resetButton{nullptr};

    // Sub-controls
    std::vector<QSlider *> m_sliders;
    std::vector<QDoubleSpinBox *> m_spinBoxes;
    QCheckBox *m_checkBox{nullptr};
    QSpinBox *m_intSpinBox{nullptr};
    QComboBox *m_comboBox{nullptr};
    QPushButton *m_colorSwatchBtn{nullptr};
    QLineEdit *m_hexEdit{nullptr};
    QLineEdit *m_textEdit{nullptr};
};

/**
 * @brief Dynamic live inspector for Wallpaper Engine scenes and Shadertoy GLSL shaders.
 * Supports auto-generated controls, 60/144 Hz parameter streaming via CommandBus & IpcClient,
 * and preset loading/saving in ~/.config/waywal/presets/<id>.toml.
 */
class PropertyInspectorWidget : public QWidget {
    Q_OBJECT

public:
    explicit PropertyInspectorWidget(QWidget *parent = nullptr);
    ~PropertyInspectorWidget() override = default;

    // --- Data Loaders ---
    void setWallpaperEngineProject(const we_project_t &proj);
    void setShaderMetadata(const waywal_shader_meta_t &meta);
    bool loadWallpaperEngineDir(const QString &workshopDir);
    bool loadShaderFile(const QString &shaderPath);
    void clear();

    // --- Configuration & Services ---
    void setTargetOutput(const QString &outputName);
    [[nodiscard]] QString targetOutput() const noexcept { return m_targetOutput; }

    void setCommandBus(CommandBus *bus) noexcept { m_commandBus = bus; }
    [[nodiscard]] CommandBus *commandBus() const noexcept { return m_commandBus; }

    void setIpcClient(IpcClient *client) noexcept { m_ipcClient = client; }
    [[nodiscard]] IpcClient *ipcClient() const noexcept { return m_ipcClient; }

    void setPresetStore(PresetStore *store) noexcept { m_presetStore = store; }
    [[nodiscard]] PresetStore *presetStore() const noexcept { return m_presetStore; }

    [[nodiscard]] const QString &currentProjectOrShaderId() const noexcept { return m_currentId; }

    // --- Preset Actions ---
    bool savePreset(const QString &presetName = QString());
    bool loadPreset(const QString &presetPathOrName);
    void resetAllToDefaults();
    [[nodiscard]] QStringList listAvailablePresets() const;

signals:
    void uniformChanged(const QString &output, const QString &name, const QVector<float> &values, uint32_t count);
    void presetSaved(const QString &presetId, const QString &filePath);
    void presetLoaded(const QString &presetId, const QString &filePath);
    void resetToDefaultsTriggered();
    void statusMessage(const QString &msg);

private slots:
    void onRowValueChanged(const QString &name, const std::vector<float> &values, uint32_t count);
    void onFilterTextChanged(const QString &text);
    void onSavePresetClicked();
    void onLoadPresetClicked();

private:
    void setupUi();
    void populateDefaultSceneParameters();
    void rebuildPropertyUi();
    void dispatchUniform(const QString &name, const std::vector<float> &oldVals,
                         const std::vector<float> &newVals, uint32_t count);
    [[nodiscard]] QString resolvePresetDir() const;
    [[nodiscard]] QString deduceCategory(const QString &name, const QString &label) const;

    QString m_targetOutput; // Empty means all outputs
    QString m_currentId{"default"};
    QString m_currentTitle{tr("No Project Loaded")};
    QString m_currentType{tr("None")};
    QString m_currentAuthor{tr("Unknown")};
    QString m_currentDesc;

    std::vector<InspectorProperty> m_properties;
    QMap<QString, PropertyRowWidget *> m_rowWidgets;

    // Injected Services
    CommandBus *m_commandBus{nullptr};
    IpcClient *m_ipcClient{nullptr};
    PresetStore *m_presetStore{nullptr};

    // UI Widgets
    QLabel *m_titleLabel{nullptr};
    QLabel *m_typeBadge{nullptr};
    QLabel *m_authorLabel{nullptr};
    QLineEdit *m_filterEdit{nullptr};
    QPushButton *m_btnSavePreset{nullptr};
    QPushButton *m_btnLoadPreset{nullptr};
    QPushButton *m_btnResetAll{nullptr};
    QScrollArea *m_scrollArea{nullptr};
    QWidget *m_containerWidget{nullptr};
    QVBoxLayout *m_containerLayout{nullptr};
};

} // namespace waywal::studio

#endif // WAYWAL_STUDIO_INSPECTOR_PROPERTY_INSPECTOR_WIDGET_HPP
