// SPDX-License-Identifier: GPL-3.0-or-later
#include "property_inspector_widget.hpp"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QStandardPaths>
#include <QTextStream>

#include <algorithm>
#include <cmath>

namespace waywal::studio {

// =============================================================================
// PropertyRowWidget Implementation
// =============================================================================

PropertyRowWidget::PropertyRowWidget(const InspectorProperty &prop, QWidget *parent)
    : QWidget(parent), m_prop(prop)
{
    setupUi();
}

void PropertyRowWidget::componentRange(int index, float &outMin, float &outMax) const noexcept
{
    outMin = (m_prop.hasRange && index >= 0 && index < static_cast<int>(m_prop.minValues.size()))
                 ? m_prop.minValues[static_cast<size_t>(index)]
                 : 0.0f;
    const float defVal = (index >= 0 && index < static_cast<int>(m_prop.defaultValues.size()))
                             ? m_prop.defaultValues[static_cast<size_t>(index)]
                             : 1.0f;
    outMax = (m_prop.hasRange && index >= 0 && index < static_cast<int>(m_prop.maxValues.size()))
                 ? m_prop.maxValues[static_cast<size_t>(index)]
                 : (defVal > 1.0f ? defVal * 2.0f : 1.0f);
    if (outMax <= outMin) {
        outMax = outMin + 1.0f;
    }
}

void PropertyRowWidget::setupUi()
{
    auto *rowLayout = new QHBoxLayout(this);
    rowLayout->setContentsMargins(2, 2, 2, 2);
    rowLayout->setSpacing(4);

    // Property Label
    QString displayLabel = !m_prop.label.isEmpty() ? m_prop.label : m_prop.name;
    m_labelWidget = new QLabel(displayLabel, this);
    m_labelWidget->setMinimumWidth(60);
    m_labelWidget->setMaximumWidth(92);
    m_labelWidget->setWordWrap(true);
    m_labelWidget->setStyleSheet("font-size: 10.5px; color: #c6d0f5;");
    m_labelWidget->setToolTip(
        QString("<b>Uniform:</b> %1<br><b>Category:</b> %2").arg(m_prop.name).arg(m_prop.category));
    rowLayout->addWidget(m_labelWidget);

    auto *controlLayout = new QHBoxLayout();
    controlLayout->setContentsMargins(0, 0, 0, 0);
    controlLayout->setSpacing(4);

    switch (m_prop.kind) {
    case InspectorProperty::Kind::Float: {
        float minVal = 0.0f;
        float maxVal = 1.0f;
        componentRange(0, minVal, maxVal);

        float stepVal = m_prop.step > 0.0001f ? m_prop.step : 0.01f;
        int decimals = 2;
        if (stepVal < 0.001f)
            decimals = 4;
        else if (stepVal < 0.01f)
            decimals = 3;

        auto *slider = new QSlider(Qt::Horizontal, this);
        slider->setRange(0, 1000);
        slider->setMinimumWidth(34);
        float cur =
            m_prop.currentValues.empty() ? m_prop.defaultValues[0] : m_prop.currentValues[0];
        int sliderPos =
            static_cast<int>(std::round(((cur - minVal) / (maxVal - minVal)) * 1000.0f));
        slider->setValue(std::clamp(sliderPos, 0, 1000));

        auto *spin = new QDoubleSpinBox(this);
        spin->setRange(minVal, maxVal);
        spin->setSingleStep(stepVal);
        spin->setDecimals(decimals);
        spin->setValue(cur);
        spin->setFixedWidth(56);
        spin->setFixedHeight(20);
        spin->setStyleSheet("font-size: 10px; padding: 1px 2px;");

        m_sliders.push_back(slider);
        m_spinBoxes.push_back(spin);

        connect(slider, &QSlider::valueChanged, this,
                [this](int val) { onSliderValueChanged(0, val); });

        connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this](double val) { onSpinValueChanged(0, val); });

        controlLayout->addWidget(slider, 1);
        controlLayout->addWidget(spin);
        break;
    }

    case InspectorProperty::Kind::Vec2:
    case InspectorProperty::Kind::Vec3:
    case InspectorProperty::Kind::Vec4: {
        uint32_t count = m_prop.numComponents;
        const char *compNames[4] = {"X:", "Y:", "Z:", "W:"};

        auto *vecStack = new QVBoxLayout();
        vecStack->setContentsMargins(0, 0, 0, 0);
        vecStack->setSpacing(2);

        for (uint32_t i = 0; i < count; ++i) {
            float minVal = 0.0f;
            float maxVal = 1.0f;
            componentRange(static_cast<int>(i), minVal, maxVal);
            float stepVal = m_prop.step > 0.0001f ? m_prop.step : 0.01f;

            auto *subLayout = new QHBoxLayout();
            subLayout->setContentsMargins(0, 0, 0, 0);
            subLayout->setSpacing(3);
            auto *cLabel = new QLabel(compNames[i], this);
            cLabel->setFixedWidth(14);
            cLabel->setStyleSheet("color: #a5adce; font-weight: bold; font-size: 9.5px;");

            auto *slider = new QSlider(Qt::Horizontal, this);
            slider->setRange(0, 1000);
            slider->setMinimumWidth(28);
            float cur = (i < m_prop.currentValues.size()) ? m_prop.currentValues[i]
                                                          : m_prop.defaultValues[i];
            int sliderPos =
                static_cast<int>(std::round(((cur - minVal) / (maxVal - minVal)) * 1000.0f));
            slider->setValue(std::clamp(sliderPos, 0, 1000));

            auto *spin = new QDoubleSpinBox(this);
            spin->setRange(minVal, maxVal);
            spin->setSingleStep(stepVal);
            spin->setDecimals(2);
            spin->setValue(cur);
            spin->setFixedWidth(54);
            spin->setFixedHeight(20);
            spin->setStyleSheet("font-size: 10px; padding: 1px 2px;");

            m_sliders.push_back(slider);
            m_spinBoxes.push_back(spin);

            int compIdx = static_cast<int>(i);
            connect(slider, &QSlider::valueChanged, this,
                    [this, compIdx](int val) { onSliderValueChanged(compIdx, val); });

            connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                    [this, compIdx](double val) { onSpinValueChanged(compIdx, val); });

            subLayout->addWidget(cLabel);
            subLayout->addWidget(slider, 1);
            subLayout->addWidget(spin);
            vecStack->addLayout(subLayout);
        }
        controlLayout->addLayout(vecStack, 1);
        break;
    }

    case InspectorProperty::Kind::Bool: {
        m_checkBox = new QCheckBox(tr("Enabled"), this);
        bool checked = (!m_prop.currentValues.empty() && m_prop.currentValues[0] > 0.5f);
        m_checkBox->setChecked(checked);
        connect(m_checkBox, &QCheckBox::toggled, this, &PropertyRowWidget::onBoolToggled);
        controlLayout->addWidget(m_checkBox);
        controlLayout->addStretch();
        break;
    }

    case InspectorProperty::Kind::Int: {
        m_intSpinBox = new QSpinBox(this);
        int minVal = m_prop.hasRange ? static_cast<int>(m_prop.minValues[0]) : 0;
        int maxVal = m_prop.hasRange ? static_cast<int>(m_prop.maxValues[0]) : 100;
        m_intSpinBox->setRange(minVal, maxVal);
        int cur = !m_prop.currentValues.empty() ? static_cast<int>(m_prop.currentValues[0])
                                                : static_cast<int>(m_prop.defaultValues[0]);
        m_intSpinBox->setValue(cur);
        m_intSpinBox->setFixedHeight(20);
        connect(m_intSpinBox, qOverload<int>(&QSpinBox::valueChanged), this,
                &PropertyRowWidget::onIntSpinChanged);
        controlLayout->addWidget(m_intSpinBox);
        controlLayout->addStretch();
        break;
    }

    case InspectorProperty::Kind::Combo: {
        m_comboBox = new QComboBox(this);
        m_comboBox->setMinimumWidth(64);
        for (const auto &opt : m_prop.comboOptions) {
            m_comboBox->addItem(opt);
        }
        if (m_prop.comboIndex >= 0 && m_prop.comboIndex < m_comboBox->count()) {
            m_comboBox->setCurrentIndex(m_prop.comboIndex);
        }
        connect(m_comboBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
                &PropertyRowWidget::onComboIndexChanged);
        controlLayout->addWidget(m_comboBox, 1);
        break;
    }

    case InspectorProperty::Kind::Color: {
        float r = (m_prop.currentValues.size() > 0) ? m_prop.currentValues[0] : 1.0f;
        float g = (m_prop.currentValues.size() > 1) ? m_prop.currentValues[1] : 1.0f;
        float b = (m_prop.currentValues.size() > 2) ? m_prop.currentValues[2] : 1.0f;
        QColor curCol = QColor::fromRgbF(std::clamp(r, 0.0f, 1.0f), std::clamp(g, 0.0f, 1.0f),
                                         std::clamp(b, 0.0f, 1.0f));

        m_colorSwatchBtn = new QPushButton(this);
        m_colorSwatchBtn->setFixedSize(36, 24);
        m_colorSwatchBtn->setCursor(Qt::PointingHandCursor);
        m_colorSwatchBtn->setToolTip(tr("Click to choose color"));
        m_colorSwatchBtn->setStyleSheet(
            QString("background-color: %1; border: 1px solid #2c3240; border-radius: 4px;")
                .arg(curCol.name(QColor::HexRgb)));

        m_hexEdit = new QLineEdit(curCol.name(QColor::HexRgb).toUpper(), this);
        m_hexEdit->setMaxLength(7);
        m_hexEdit->setFixedWidth(68);
        m_hexEdit->setFixedHeight(24);
        m_hexEdit->setStyleSheet("font-size: 11px; padding: 2px 4px;");

        connect(m_colorSwatchBtn, &QPushButton::clicked, this,
                &PropertyRowWidget::onColorButtonClicked);
        connect(m_hexEdit, &QLineEdit::textEdited, this, &PropertyRowWidget::onHexEdited);

        controlLayout->addWidget(m_colorSwatchBtn);
        controlLayout->addWidget(m_hexEdit);
        controlLayout->addStretch();
        break;
    }

    case InspectorProperty::Kind::Text: {
        m_textEdit = new QLineEdit(m_prop.textValue, this);
        m_textEdit->setMinimumWidth(60);
        connect(m_textEdit, &QLineEdit::textEdited, this, &PropertyRowWidget::onTextEdited);
        controlLayout->addWidget(m_textEdit, 1);
        break;
    }
    }

    rowLayout->addLayout(controlLayout, 1);

    // Reset-to-default button
    m_resetButton = new QToolButton(this);
    m_resetButton->setText(tr("Reset"));
    m_resetButton->setFixedSize(44, 22);
    m_resetButton->setToolTip(tr("Reset this parameter to default value"));
    m_resetButton->setStyleSheet(
        "QToolButton { color: #94a3b8; font-size: 10px; background: #20242e; border: 1px solid "
        "#2c3240; border-radius: 4px; padding: 1px 4px; } "
        "QToolButton:hover { color: #f1f5f9; background: #2a303d; }");
    connect(m_resetButton, &QToolButton::clicked, this, &PropertyRowWidget::resetToDefault);
    rowLayout->addWidget(m_resetButton);
}

void PropertyRowWidget::syncSliderSpin(int index, float val, bool emitSignal)
{
    if (index < 0 || index >= static_cast<int>(m_prop.currentValues.size()))
        return;

    float minVal = 0.0f;
    float maxVal = 1.0f;
    componentRange(index, minVal, maxVal);

    {
        const QScopedValueRollback<bool> guard(m_updating, true);
        m_prop.currentValues[static_cast<size_t>(index)] = val;

        if (index < static_cast<int>(m_sliders.size())) {
            int sliderPos =
                static_cast<int>(std::round(((val - minVal) / (maxVal - minVal)) * 1000.0f));
            m_sliders[static_cast<size_t>(index)]->setValue(std::clamp(sliderPos, 0, 1000));
        }
        if (index < static_cast<int>(m_spinBoxes.size())) {
            m_spinBoxes[static_cast<size_t>(index)]->setValue(val);
        }
    }

    if (emitSignal) {
        emitChange();
    }
}

void PropertyRowWidget::onSliderValueChanged(int index, int sliderVal)
{
    if (m_updating)
        return;
    float minVal = 0.0f;
    float maxVal = 1.0f;
    componentRange(index, minVal, maxVal);
    float fval = minVal + (static_cast<float>(sliderVal) / 1000.0f) * (maxVal - minVal);
    syncSliderSpin(index, fval);
}

void PropertyRowWidget::onSpinValueChanged(int index, double spinVal)
{
    if (m_updating)
        return;
    syncSliderSpin(index, static_cast<float>(spinVal));
}

void PropertyRowWidget::onBoolToggled(bool checked)
{
    if (m_updating)
        return;
    if (m_prop.currentValues.empty())
        m_prop.currentValues.resize(1);
    m_prop.currentValues[0] = checked ? 1.0f : 0.0f;
    emitChange();
}

void PropertyRowWidget::onIntSpinChanged(int val)
{
    if (m_updating)
        return;
    if (m_prop.currentValues.empty())
        m_prop.currentValues.resize(1);
    m_prop.currentValues[0] = static_cast<float>(val);
    emitChange();
}

void PropertyRowWidget::onComboIndexChanged(int idx)
{
    if (m_updating)
        return;
    m_prop.comboIndex = idx;
    if (m_prop.currentValues.empty())
        m_prop.currentValues.resize(1);
    m_prop.currentValues[0] = static_cast<float>(idx);
    emitChange();
}

void PropertyRowWidget::onColorButtonClicked()
{
    float r = (m_prop.currentValues.size() > 0) ? m_prop.currentValues[0] : 1.0f;
    float g = (m_prop.currentValues.size() > 1) ? m_prop.currentValues[1] : 1.0f;
    float b = (m_prop.currentValues.size() > 2) ? m_prop.currentValues[2] : 1.0f;
    QColor initCol = QColor::fromRgbF(std::clamp(r, 0.0f, 1.0f), std::clamp(g, 0.0f, 1.0f),
                                      std::clamp(b, 0.0f, 1.0f));

    OklchColorDialog dlg(initCol, this);
    dlg.setWindowTitle(
        tr("Pick Color - %1").arg(m_prop.label.isEmpty() ? m_prop.name : m_prop.label));

    // Connect live changes to stream parameters while dragging in color picker
    connect(&dlg, &OklchColorDialog::colorChanged, this, [this](const QColor &newCol) {
        if (m_prop.currentValues.size() < 3)
            m_prop.currentValues.resize(3);
        m_prop.currentValues[0] = newCol.redF();
        m_prop.currentValues[1] = newCol.greenF();
        m_prop.currentValues[2] = newCol.blueF();

        if (m_colorSwatchBtn) {
            m_colorSwatchBtn->setStyleSheet(
                QString("background-color: %1; border: 1px solid #2c3240; border-radius: 4px;")
                    .arg(newCol.name(QColor::HexRgb)));
        }
        if (m_hexEdit) {
            m_hexEdit->setText(newCol.name(QColor::HexRgb).toUpper());
        }
        emitChange();
    });

    if (dlg.exec() == QDialog::Rejected) {
        // Restore initial color on cancel
        m_prop.currentValues[0] = initCol.redF();
        m_prop.currentValues[1] = initCol.greenF();
        m_prop.currentValues[2] = initCol.blueF();
        if (m_colorSwatchBtn) {
            m_colorSwatchBtn->setStyleSheet(
                QString("background-color: %1; border: 1px solid #2c3240; border-radius: 4px;")
                    .arg(initCol.name(QColor::HexRgb)));
        }
        if (m_hexEdit) {
            m_hexEdit->setText(initCol.name(QColor::HexRgb).toUpper());
        }
        emitChange();
    }
}

void PropertyRowWidget::onHexEdited(const QString &text)
{
    if (m_updating)
        return;
    QString cleaned = text.trimmed();
    if (!cleaned.startsWith('#'))
        cleaned.prepend('#');
    if (QColor::isValidColorName(cleaned)) {
        QColor col(cleaned);
        if (m_prop.currentValues.size() < 3)
            m_prop.currentValues.resize(3);
        m_prop.currentValues[0] = col.redF();
        m_prop.currentValues[1] = col.greenF();
        m_prop.currentValues[2] = col.blueF();

        if (m_colorSwatchBtn) {
            m_colorSwatchBtn->setStyleSheet(
                QString("background-color: %1; border: 1px solid #2c3240; border-radius: 4px;")
                    .arg(col.name(QColor::HexRgb)));
        }
        emitChange();
    }
}

void PropertyRowWidget::onTextEdited(const QString &text)
{
    if (m_updating)
        return;
    m_prop.textValue = text;
    // Dispatches notification for text properties
    emitChange();
}

void PropertyRowWidget::emitChange()
{
    emit valueChanged(m_prop.name, m_prop.currentValues, m_prop.numComponents);
}

void PropertyRowWidget::updateValues(const std::vector<float> &vals, bool emitSignal)
{
    {
        const QScopedValueRollback<bool> guard(m_updating, true);
        m_prop.currentValues = vals;

        if (m_prop.kind == InspectorProperty::Kind::Float && !vals.empty()) {
            syncSliderSpin(0, vals[0], false);
        } else if (m_prop.kind == InspectorProperty::Kind::Bool && !vals.empty()) {
            if (m_checkBox)
                m_checkBox->setChecked(vals[0] > 0.5f);
        } else if (m_prop.kind == InspectorProperty::Kind::Int && !vals.empty()) {
            if (m_intSpinBox)
                m_intSpinBox->setValue(static_cast<int>(vals[0]));
        } else if (m_prop.kind == InspectorProperty::Kind::Combo && !vals.empty()) {
            if (m_comboBox)
                m_comboBox->setCurrentIndex(static_cast<int>(vals[0]));
        } else if (m_prop.kind == InspectorProperty::Kind::Color && vals.size() >= 3) {
            QColor col =
                QColor::fromRgbF(std::clamp(vals[0], 0.0f, 1.0f), std::clamp(vals[1], 0.0f, 1.0f),
                                 std::clamp(vals[2], 0.0f, 1.0f));
            if (m_colorSwatchBtn) {
                m_colorSwatchBtn->setStyleSheet(
                    QString("background-color: %1; border: 1px solid #2c3240; border-radius: 4px;")
                        .arg(col.name(QColor::HexRgb)));
            }
            if (m_hexEdit) {
                m_hexEdit->setText(col.name(QColor::HexRgb).toUpper());
            }
        } else {
            for (size_t i = 0; i < vals.size() && i < m_sliders.size(); ++i) {
                syncSliderSpin(static_cast<int>(i), vals[i], false);
            }
        }
    }

    if (emitSignal) {
        emitChange();
    }
}

void PropertyRowWidget::resetToDefault() { updateValues(m_prop.defaultValues); }

void PropertyRowWidget::setFilterVisible(bool visible) { setVisible(visible); }

// =============================================================================
// PropertyInspectorWidget Implementation
// =============================================================================

PropertyInspectorWidget::PropertyInspectorWidget(QWidget *parent) : QWidget(parent)
{
    setupUi();
    populateDefaultSceneParameters();
}

void PropertyInspectorWidget::setupUi()
{
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(10, 8, 10, 8);
    rootLayout->setSpacing(8);

    // --- Header Card ---
    auto *headerWidget = new QWidget(this);
    headerWidget->setStyleSheet(
        "background: #181b22; border: 1px solid #2c3240; border-radius: 6px;");
    auto *headerLayout = new QVBoxLayout(headerWidget);
    headerLayout->setContentsMargins(10, 8, 10, 8);
    headerLayout->setSpacing(4);

    auto *topRow = new QHBoxLayout();
    topRow->setSpacing(8);
    m_titleLabel = new QLabel(m_currentTitle, headerWidget);
    m_titleLabel->setMinimumWidth(60);
    m_titleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_titleLabel->setStyleSheet("font-size: 13px; font-weight: bold; color: #f1f5f9;");

    m_typeBadge = new QLabel(m_currentType, headerWidget);
    m_typeBadge->setStyleSheet("background: #0284c7; color: #ffffff; font-weight: bold; "
                               "border-radius: 4px; padding: 2px 8px; font-size: 10px;");

    topRow->addWidget(m_titleLabel, 1);
    topRow->addWidget(m_typeBadge, 0);
    headerLayout->addLayout(topRow);

    m_authorLabel = new QLabel(headerWidget);
    m_authorLabel->setMinimumWidth(50);
    m_authorLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_authorLabel->setStyleSheet("color: #94a3b8; font-size: 11px;");
    m_authorLabel->setVisible(false);
    headerLayout->addWidget(m_authorLabel);

    rootLayout->addWidget(headerWidget);

    // --- Toolbar: Search Filter & Presets ---
    m_filterEdit = new QLineEdit(this);
    m_filterEdit->setPlaceholderText(tr("Filter parameters..."));
    m_filterEdit->setClearButtonEnabled(true);
    m_filterEdit->setFixedHeight(26);
    connect(m_filterEdit, &QLineEdit::textChanged, this,
            &PropertyInspectorWidget::onFilterTextChanged);
    rootLayout->addWidget(m_filterEdit);

    auto *toolBar = new QHBoxLayout();
    toolBar->setContentsMargins(0, 0, 0, 0);
    toolBar->setSpacing(8);

    m_btnSavePreset = new QPushButton(tr("Save"), this);
    m_btnSavePreset->setFixedHeight(26);
    m_btnSavePreset->setToolTip(tr("Save current parameters as a named preset"));
    m_btnSavePreset->setStyleSheet(
        "background: #20242e; color: #f1f5f9; border: 1px solid #2c3240; padding: 4px 10px; "
        "border-radius: 5px; font-size: 11px;");
    connect(m_btnSavePreset, &QPushButton::clicked, this,
            &PropertyInspectorWidget::onSavePresetClicked);
    toolBar->addWidget(m_btnSavePreset, 1);

    m_btnLoadPreset = new QPushButton(tr("Load"), this);
    m_btnLoadPreset->setFixedHeight(26);
    m_btnLoadPreset->setToolTip(tr("Load parameters from a saved preset"));
    m_btnLoadPreset->setStyleSheet(
        "background: #20242e; color: #f1f5f9; border: 1px solid #2c3240; padding: 4px 10px; "
        "border-radius: 5px; font-size: 11px;");
    connect(m_btnLoadPreset, &QPushButton::clicked, this,
            &PropertyInspectorWidget::onLoadPresetClicked);
    toolBar->addWidget(m_btnLoadPreset, 1);

    m_btnResetAll = new QPushButton(tr("Reset"), this);
    m_btnResetAll->setFixedHeight(26);
    m_btnResetAll->setToolTip(tr("Reset all parameters to their default values"));
    m_btnResetAll->setStyleSheet(
        "background: #20242e; color: #f87171; border: 1px solid #2c3240; padding: 4px 10px; "
        "border-radius: 5px; font-size: 11px;");
    connect(m_btnResetAll, &QPushButton::clicked, this,
            &PropertyInspectorWidget::resetAllToDefaults);
    toolBar->addWidget(m_btnResetAll, 1);

    rootLayout->addLayout(toolBar);

    // --- Scroll Area ---
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setStyleSheet("QScrollArea { background: transparent; }");

    m_containerWidget = new QWidget(m_scrollArea);
    m_containerLayout = new QVBoxLayout(m_containerWidget);
    m_containerLayout->setContentsMargins(0, 0, 4, 0);
    m_containerLayout->setSpacing(8);

    m_scrollArea->setWidget(m_containerWidget);
    rootLayout->addWidget(m_scrollArea, 1);
    setMinimumWidth(200);
}

void PropertyInspectorWidget::populateDefaultSceneParameters()
{
    m_currentId = QStringLiteral("studio_default_scene");
    m_currentTitle = tr("Shader Parameters");
    m_currentType = QStringLiteral("GLES 3.1");
    m_titleLabel->setText(m_currentTitle);
    m_typeBadge->setText(m_currentType);
    m_authorLabel->setText(tr("Live Uniforms"));
    m_authorLabel->setVisible(true);

    m_properties.clear();

    InspectorProperty speedProp;
    speedProp.id = QStringLiteral("u_speed");
    speedProp.name = QStringLiteral("u_speed");
    speedProp.label = tr("Time Speed");
    speedProp.category = tr("Motion & Physics");
    speedProp.kind = InspectorProperty::Kind::Float;
    speedProp.numComponents = 1;
    speedProp.currentValues = {1.0f};
    speedProp.defaultValues = {1.0f};
    speedProp.minValues = {0.0f};
    speedProp.maxValues = {4.0f};
    speedProp.step = 0.05f;
    speedProp.hasRange = true;
    m_properties.push_back(speedProp);

    InspectorProperty parallaxProp;
    parallaxProp.id = QStringLiteral("u_parallax_depth");
    parallaxProp.name = QStringLiteral("u_parallax_depth");
    parallaxProp.label = tr("Parallax Depth");
    parallaxProp.category = tr("Motion & Physics");
    parallaxProp.kind = InspectorProperty::Kind::Float;
    parallaxProp.numComponents = 1;
    parallaxProp.currentValues = {0.35f};
    parallaxProp.defaultValues = {0.35f};
    parallaxProp.minValues = {0.0f};
    parallaxProp.maxValues = {1.0f};
    parallaxProp.step = 0.02f;
    parallaxProp.hasRange = true;
    m_properties.push_back(parallaxProp);

    InspectorProperty bloomProp;
    bloomProp.id = QStringLiteral("u_bloom_intensity");
    bloomProp.name = QStringLiteral("u_bloom_intensity");
    bloomProp.label = tr("Bloom Glow");
    bloomProp.category = tr("Post-Processing & FX");
    bloomProp.kind = InspectorProperty::Kind::Float;
    bloomProp.numComponents = 1;
    bloomProp.currentValues = {0.65f};
    bloomProp.defaultValues = {0.65f};
    bloomProp.minValues = {0.0f};
    bloomProp.maxValues = {2.0f};
    bloomProp.step = 0.05f;
    bloomProp.hasRange = true;
    m_properties.push_back(bloomProp);

    InspectorProperty tintProp;
    tintProp.id = QStringLiteral("u_accent_color");
    tintProp.name = QStringLiteral("u_accent_color");
    tintProp.label = tr("Accent Tint");
    tintProp.category = tr("Colors & Lighting");
    tintProp.kind = InspectorProperty::Kind::Color;
    tintProp.numComponents = 3;
    tintProp.currentValues = {0.35f, 0.74f, 0.97f};
    tintProp.defaultValues = {0.35f, 0.74f, 0.97f};
    tintProp.minValues = {0.0f, 0.0f, 0.0f};
    tintProp.maxValues = {1.0f, 1.0f, 1.0f};
    tintProp.hasRange = true;
    m_properties.push_back(tintProp);

    InspectorProperty audioProp;
    audioProp.id = QStringLiteral("u_audio_reactivity");
    audioProp.name = QStringLiteral("u_audio_reactivity");
    audioProp.label = tr("Audio Gain");
    audioProp.category = tr("Audio & Reactivity");
    audioProp.kind = InspectorProperty::Kind::Float;
    audioProp.numComponents = 1;
    audioProp.currentValues = {1.0f};
    audioProp.defaultValues = {1.0f};
    audioProp.minValues = {0.0f};
    audioProp.maxValues = {3.0f};
    audioProp.step = 0.05f;
    audioProp.hasRange = true;
    m_properties.push_back(audioProp);

    rebuildPropertyUi();
}

void PropertyInspectorWidget::setTargetOutput(const QString &outputName)
{
    m_targetOutput = outputName;
}

QString PropertyInspectorWidget::deduceCategory(const QString &name, const QString &label) const
{
    QString combined = (name + " " + label).toLower();

    if (combined.contains("color") || combined.contains("tint") || combined.contains("scheme") ||
        combined.contains("palette") || combined.contains("albedo") ||
        combined.contains("diffuse")) {
        return tr("Colors & Lighting");
    }
    if (combined.contains("speed") || combined.contains("time") || combined.contains("rate") ||
        combined.contains("fps") || combined.contains("velocity") ||
        combined.contains("frequency") || combined.contains("motion") ||
        combined.contains("duration")) {
        return tr("Motion & Timing");
    }
    if (combined.contains("bloom") || combined.contains("glow") || combined.contains("blur") ||
        combined.contains("noise") || combined.contains("grain") ||
        combined.contains("distortion") || combined.contains("vignette") ||
        combined.contains("wave") || combined.contains("shadow")) {
        return tr("Visual Effects");
    }
    if (combined.contains("camera") || combined.contains("fov") || combined.contains("zoom") ||
        combined.contains("pos") || combined.contains("scale") || combined.contains("rotate") ||
        combined.contains("angle") || combined.contains("offset")) {
        return tr("Geometry & Camera");
    }
    return tr("General Parameters");
}

void PropertyInspectorWidget::clear()
{
    m_properties.clear();
    m_rowWidgets.clear();

    QLayoutItem *item = nullptr;
    while ((item = m_containerLayout->takeAt(0)) != nullptr) {
        if (item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }

    m_currentId = "default";
    m_currentTitle = tr("No Project Loaded");
    m_currentType = tr("None");
    m_titleLabel->setText(m_currentTitle);
    m_typeBadge->setText(m_currentType);
    m_authorLabel->setVisible(false);
}

void PropertyInspectorWidget::setWallpaperEngineProject(const we_project_t &proj)
{
    clear();

    m_currentId = QString::fromUtf8(proj.workshop_id);
    if (m_currentId.isEmpty()) {
        m_currentId = QFileInfo(QString::fromUtf8(proj.workshop_dir)).fileName();
    }
    if (m_currentId.isEmpty())
        m_currentId = "we_project";

    m_currentTitle = QString::fromUtf8(proj.title);
    if (m_currentTitle.isEmpty())
        m_currentTitle = tr("Wallpaper Engine Project");
    m_currentType = QString::fromUtf8(proj.type).toUpper();
    if (m_currentType.isEmpty())
        m_currentType = "SCENE";

    m_titleLabel->setText(m_currentTitle);
    m_typeBadge->setText(m_currentType);
    m_typeBadge->setStyleSheet("background: #a6d189; color: #1e1e2e; font-weight: bold; "
                               "border-radius: 3px; padding: 2px 6px; font-size: 11px;");

    for (uint32_t i = 0; i < proj.prop_count && i < WE_MAX_USER_PROPS; ++i) {
        const we_user_prop_t &p = proj.props[i];
        InspectorProperty prop;
        prop.id = QString::fromUtf8(p.key);
        prop.name = QString::fromUtf8(p.key);
        prop.label = QString::fromUtf8(p.text);
        if (prop.label.isEmpty())
            prop.label = prop.name;
        prop.category = deduceCategory(prop.name, prop.label);
        prop.numComponents = p.num_components > 0 ? p.num_components : 1;

        prop.currentValues.resize(prop.numComponents);
        prop.defaultValues.resize(prop.numComponents);
        prop.minValues.resize(prop.numComponents, 0.0f);
        prop.maxValues.resize(prop.numComponents, 1.0f);

        for (uint32_t c = 0; c < prop.numComponents; ++c) {
            prop.currentValues[c] = p.val[c];
            prop.defaultValues[c] = p.val[c];
        }

        switch (p.type) {
        case WE_PROP_FLOAT:
            prop.kind = InspectorProperty::Kind::Float;
            if (prop.defaultValues[0] > 1.0f) {
                prop.maxValues[0] = std::max(10.0f, prop.defaultValues[0] * 2.0f);
            }
            break;
        case WE_PROP_BOOL:
            prop.kind = InspectorProperty::Kind::Bool;
            break;
        case WE_PROP_INT:
            prop.kind = InspectorProperty::Kind::Int;
            prop.maxValues[0] = std::max(100.0f, prop.defaultValues[0] * 2.0f);
            break;
        case WE_PROP_COLOR:
            prop.kind = InspectorProperty::Kind::Color;
            prop.numComponents = 3;
            if (prop.currentValues.size() < 3)
                prop.currentValues.resize(3, 1.0f);
            if (prop.defaultValues.size() < 3)
                prop.defaultValues.resize(3, 1.0f);
            break;
        case WE_PROP_TEXT:
            prop.kind = InspectorProperty::Kind::Text;
            prop.textValue = QString::fromUtf8(p.str_val);
            prop.defaultTextValue = prop.textValue;
            break;
        }

        m_properties.push_back(prop);
    }

    rebuildPropertyUi();
}

void PropertyInspectorWidget::setShaderMetadata(const waywal_shader_meta_t &meta)
{
    clear();

    m_currentTitle = QString::fromUtf8(meta.title);
    if (m_currentTitle.isEmpty())
        m_currentTitle = tr("GLSL Shader");

    m_currentId = m_currentTitle.toLower().replace(QRegularExpression("[^a-z0-9_]+"), "_");
    m_currentType = meta.has_multi_pass ? "MULTI-PASS GLSL" : "COMPUTE GLSL";

    m_titleLabel->setText(m_currentTitle);
    m_typeBadge->setText(m_currentType);
    m_typeBadge->setStyleSheet("background: #ef9f76; color: #1e1e2e; font-weight: bold; "
                               "border-radius: 3px; padding: 2px 6px; font-size: 11px;");

    QString author = QString::fromUtf8(meta.author);
    if (!author.isEmpty()) {
        m_authorLabel->setText(tr("Author: %1").arg(author));
        m_authorLabel->setVisible(true);
    }

    for (uint32_t i = 0; i < meta.uniform_count && i < WAYWAL_MAX_SHADER_UNIFORMS; ++i) {
        const waywal_shader_uniform_def_t &u = meta.uniforms[i];
        InspectorProperty prop;
        prop.id = QString::fromUtf8(u.name);
        prop.name = QString::fromUtf8(u.name);
        prop.label = QString::fromUtf8(u.label);
        if (prop.label.isEmpty())
            prop.label = prop.name;
        prop.category = deduceCategory(prop.name, prop.label);
        prop.numComponents = u.num_components > 0 ? u.num_components : 1;
        prop.step = u.step > 0.0001f ? u.step : 0.01f;
        prop.hasRange = u.has_range;

        prop.currentValues.resize(prop.numComponents);
        prop.defaultValues.resize(prop.numComponents);
        prop.minValues.resize(prop.numComponents);
        prop.maxValues.resize(prop.numComponents);

        for (uint32_t c = 0; c < prop.numComponents; ++c) {
            prop.currentValues[c] = u.default_val[c];
            prop.defaultValues[c] = u.default_val[c];
            prop.minValues[c] = u.min_val[c];
            prop.maxValues[c] = u.max_val[c];
        }

        switch (u.type) {
        case WAYWAL_UNIFORM_TYPE_FLOAT:
            prop.kind = InspectorProperty::Kind::Float;
            break;
        case WAYWAL_UNIFORM_TYPE_BOOL:
            prop.kind = InspectorProperty::Kind::Bool;
            break;
        case WAYWAL_UNIFORM_TYPE_INT:
            prop.kind = InspectorProperty::Kind::Int;
            break;
        case WAYWAL_UNIFORM_TYPE_VEC2:
            prop.kind = InspectorProperty::Kind::Vec2;
            break;
        case WAYWAL_UNIFORM_TYPE_VEC3:
            prop.kind = InspectorProperty::Kind::Vec3;
            break;
        case WAYWAL_UNIFORM_TYPE_VEC4:
            prop.kind = InspectorProperty::Kind::Vec4;
            break;
        case WAYWAL_UNIFORM_TYPE_COLOR:
            prop.kind = InspectorProperty::Kind::Color;
            break;
        }

        m_properties.push_back(prop);
    }

    rebuildPropertyUi();
}

bool PropertyInspectorWidget::loadWallpaperEngineDir(const QString &workshopDir)
{
    we_project_t proj{};
    if (we_project_load_dir(workshopDir.toUtf8().constData(), &proj)) {
        setWallpaperEngineProject(proj);
        return true;
    }
    return false;
}

bool PropertyInspectorWidget::loadShaderFile(const QString &shaderPath)
{
    waywal_shader_meta_t meta;
    waywal_shader_meta_init(&meta);
    if (waywal_shader_parse_file(shaderPath.toUtf8().constData(), &meta)) {
        setShaderMetadata(meta);
        return true;
    }
    return false;
}

void PropertyInspectorWidget::rebuildPropertyUi()
{
    // Clear old container widgets
    QLayoutItem *item = nullptr;
    while ((item = m_containerLayout->takeAt(0)) != nullptr) {
        if (item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }
    m_rowWidgets.clear();

    if (m_properties.empty()) {
        auto *emptyLabel =
            new QLabel(tr("<i>No editable parameters found.</i>"), m_containerWidget);
        emptyLabel->setAlignment(Qt::AlignCenter);
        emptyLabel->setStyleSheet("color: #737994; padding: 24px; font-size: 13px;");
        m_containerLayout->addWidget(emptyLabel);
        return;
    }

    // Group properties by category
    QMap<QString, std::vector<InspectorProperty>> categorized;
    for (const auto &prop : m_properties) {
        categorized[prop.category].push_back(prop);
    }

    for (auto it = categorized.begin(); it != categorized.end(); ++it) {
        const QString &catName = it.key();
        const auto &propList = it.value();

        QString cleanTitle = catName;
        cleanTitle.replace(QLatin1Char('_'), QLatin1Char(' '));
        cleanTitle.replace(QLatin1String("&"), QLatin1String("&&"));

        auto *group = new QGroupBox(cleanTitle, m_containerWidget);
        group->setStyleSheet(
            "QGroupBox { font-weight: bold; font-size: 11px; border: 1px solid #2c3240; "
            "border-radius: 6px; "
            "margin-top: 8px; padding-top: 10px; background: #181b22; } "
            "QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; "
            "left: 10px; top: 0px; padding: 0px 6px 2px 6px; color: #38bdf8; }");

        auto *groupLayout = new QVBoxLayout(group);
        groupLayout->setContentsMargins(8, 6, 8, 6);
        groupLayout->setSpacing(6);

        for (const auto &p : propList) {
            auto *row = new PropertyRowWidget(p, group);
            connect(row, &PropertyRowWidget::valueChanged, this,
                    &PropertyInspectorWidget::onRowValueChanged);
            m_rowWidgets[p.name] = row;
            groupLayout->addWidget(row);
        }

        m_containerLayout->addWidget(group);
    }

    m_containerLayout->addStretch(1);
}

std::vector<float> PropertyInspectorWidget::propertyValues(const QString &name) const
{
    for (const auto &prop : m_properties) {
        if (prop.name == name) {
            return prop.currentValues;
        }
    }
    return {};
}

void PropertyInspectorWidget::onRowValueChanged(const QString &name,
                                                const std::vector<float> &values, uint32_t count)
{
    std::vector<float> oldValues;
    for (auto &prop : m_properties) {
        if (prop.name == name) {
            oldValues = prop.currentValues;
            prop.currentValues = values;
            break;
        }
    }

    dispatchUniform(name, oldValues, values, count);
}

void PropertyInspectorWidget::dispatchUniform(const QString &name,
                                              const std::vector<float> &oldVals,
                                              const std::vector<float> &newVals, uint32_t count)
{
    if (m_commandBus) {
        QPointer<PropertyInspectorWidget> self(this);
        auto cmd = std::make_unique<SetUniformCommand>(
            m_targetOutput, name, oldVals, newVals, 0,
            [self](const QString &out, const QString &nm, const float *vals, uint32_t cnt,
                   uint32_t) {
                if (!self) {
                    return;
                }
                std::vector<float> vec(vals, vals + cnt);
                for (auto &prop : self->m_properties) {
                    if (prop.name == nm) {
                        prop.currentValues = vec;
                        break;
                    }
                }
                if (auto it = self->m_rowWidgets.find(nm); it != self->m_rowWidgets.end() && *it) {
                    (*it)->updateValues(vec, false);
                }
                QVector<float> qvals(vec.begin(), vec.end());
                emit self->uniformChanged(out, nm, qvals, cnt);
                if (self->m_ipcClient) {
                    self->m_ipcClient->sendSetUniform(out, nm, vals, cnt);
                }
            });
        m_commandBus->execute(std::move(cmd));
    } else {
        QVector<float> qvals(newVals.begin(), newVals.end());
        emit uniformChanged(m_targetOutput, name, qvals, count);
        if (m_ipcClient) {
            m_ipcClient->sendSetUniform(m_targetOutput, name, newVals.data(), count);
        }
    }
}

void PropertyInspectorWidget::onFilterTextChanged(const QString &text)
{
    QString query = text.trimmed().toLower();
    for (auto it = m_rowWidgets.begin(); it != m_rowWidgets.end(); ++it) {
        PropertyRowWidget *row = it.value();
        const InspectorProperty &p = row->property();
        bool match = query.isEmpty() || p.name.toLower().contains(query) ||
                     p.label.toLower().contains(query) || p.category.toLower().contains(query);
        row->setFilterVisible(match);
    }
}

QString PropertyInspectorWidget::resolvePresetDir() const
{
    QString xdgConfig = QString::fromUtf8(qgetenv("XDG_CONFIG_HOME"));
    if (xdgConfig.isEmpty()) {
        xdgConfig = QDir::homePath() + "/.config";
    }
    QString dir = xdgConfig + QStringLiteral("/wwal/presets");
    QDir().mkpath(dir);
    return dir;
}

bool PropertyInspectorWidget::savePreset(const QString &presetName)
{
    QString name = presetName;
    if (name.isEmpty()) {
        bool ok = false;
        name = QInputDialog::getText(this, tr("Save Preset"), tr("Enter a name for this preset:"),
                                     QLineEdit::Normal, m_currentId + "_preset", &ok);
        if (!ok || name.trimmed().isEmpty()) {
            return false;
        }
    }

    QString safeId = name.toLower().replace(QRegularExpression("[^a-z0-9_]+"), "_");
    QString presetDir = resolvePresetDir();
    QString filePath = presetDir + "/" + safeId + ".toml";

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        emit statusMessage(tr("Failed to save preset: cannot open file %1").arg(filePath));
        return false;
    }

    QTextStream out(&file);
    out << "# WayWal Parameter Preset\n";
    out << "[preset]\n";
    out << "id = \"" << safeId << "\"\n";
    out << "target_project = \"" << m_currentId << "\"\n";
    out << "title = \"" << QString(m_currentTitle).replace('"', "\\\"") << "\"\n";
    out << "created_at = \"" << QDateTime::currentDateTimeUtc().toString(Qt::ISODate) << "\"\n\n";

    out << "[uniforms]\n";
    for (const auto &p : m_properties) {
        if (p.kind == InspectorProperty::Kind::Color && p.currentValues.size() >= 3) {
            out << p.name << " = [" << QString::number(p.currentValues[0], 'f', 6) << ", "
                << QString::number(p.currentValues[1], 'f', 6) << ", "
                << QString::number(p.currentValues[2], 'f', 6) << "]\n";
        } else if (p.numComponents > 1) {
            out << p.name << " = [";
            for (size_t i = 0; i < p.currentValues.size(); ++i) {
                if (i > 0)
                    out << ", ";
                out << QString::number(p.currentValues[i], 'f', 6);
            }
            out << "]\n";
        } else if (p.kind == InspectorProperty::Kind::Bool) {
            out << p.name << " = "
                << (!p.currentValues.empty() && p.currentValues[0] > 0.5f ? "true" : "false")
                << "\n";
        } else if (p.kind == InspectorProperty::Kind::Int ||
                   p.kind == InspectorProperty::Kind::Combo) {
            out << p.name << " = "
                << static_cast<int>(!p.currentValues.empty() ? p.currentValues[0] : 0.0f) << "\n";
        } else if (p.kind == InspectorProperty::Kind::Text) {
            out << p.name << " = \"" << QString(p.textValue).replace('"', "\\\"") << "\"\n";
        } else {
            float val = !p.currentValues.empty() ? p.currentValues[0] : 0.0f;
            out << p.name << " = " << QString::number(val, 'f', 6) << "\n";
        }
    }

    file.close();
    emit presetSaved(safeId, filePath);
    emit statusMessage(tr("Preset saved to %1").arg(filePath));
    return true;
}

bool PropertyInspectorWidget::loadPreset(const QString &presetPathOrName)
{
    QString filePath = presetPathOrName;
    if (!QFileInfo::exists(filePath)) {
        filePath = resolvePresetDir() + "/" + presetPathOrName;
        if (!filePath.endsWith(".toml"))
            filePath += ".toml";
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        emit statusMessage(tr("Failed to load preset: file not found %1").arg(filePath));
        return false;
    }

    QTextStream in(&file);
    bool inUniforms = false;

    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;

        if (line.startsWith('[')) {
            inUniforms = (line == "[uniforms]");
            continue;
        }

        if (inUniforms) {
            int eqIdx = line.indexOf('=');
            if (eqIdx <= 0)
                continue;

            QString key = line.left(eqIdx).trimmed();
            QString valStr = line.mid(eqIdx + 1).trimmed();

            if (!m_rowWidgets.contains(key))
                continue;
            PropertyRowWidget *row = m_rowWidgets[key];

            std::vector<float> parsedVals;
            if (valStr.startsWith('[') && valStr.endsWith(']')) {
                QString inner = valStr.mid(1, valStr.length() - 2);
                QStringList parts = inner.split(',', Qt::SkipEmptyParts);
                for (const auto &p : parts) {
                    bool ok = false;
                    float f = p.trimmed().toFloat(&ok);
                    if (ok)
                        parsedVals.push_back(f);
                }
            } else if (valStr == "true") {
                parsedVals.push_back(1.0f);
            } else if (valStr == "false") {
                parsedVals.push_back(0.0f);
            } else {
                bool ok = false;
                float f = valStr.toFloat(&ok);
                if (ok) {
                    parsedVals.push_back(f);
                }
            }

            if (!parsedVals.empty()) {
                row->updateValues(parsedVals);
            }
        }
    }

    file.close();
    QString presetId = QFileInfo(filePath).baseName();
    emit presetLoaded(presetId, filePath);
    emit statusMessage(tr("Preset loaded: %1").arg(presetId));
    return true;
}

void PropertyInspectorWidget::resetAllToDefaults()
{
    for (auto it = m_rowWidgets.begin(); it != m_rowWidgets.end(); ++it) {
        it.value()->resetToDefault();
    }
    emit resetToDefaultsTriggered();
    emit statusMessage(tr("All parameters reset to default values."));
}

QStringList PropertyInspectorWidget::listAvailablePresets() const
{
    QDir dir(resolvePresetDir());
    return dir.entryList({"*.toml"}, QDir::Files, QDir::Time);
}

void PropertyInspectorWidget::onSavePresetClicked() { savePreset(); }

void PropertyInspectorWidget::onLoadPresetClicked()
{
    QStringList presets = listAvailablePresets();
    if (presets.isEmpty()) {
        QString filePath = QFileDialog::getOpenFileName(this, tr("Load Preset"), resolvePresetDir(),
                                                        tr("TOML Files (*.toml)"));
        if (!filePath.isEmpty()) {
            loadPreset(filePath);
        }
        return;
    }

    auto *menu = new QMenu(this);
    for (const auto &presetFile : presets) {
        QString id = QFileInfo(presetFile).baseName();
        auto *action = menu->addAction(id);
        connect(action, &QAction::triggered, this,
                [this, presetFile]() { loadPreset(presetFile); });
    }
    menu->addSeparator();
    auto *browseAction = menu->addAction(tr("Browse other..."));
    connect(browseAction, &QAction::triggered, this, [this]() {
        QString filePath = QFileDialog::getOpenFileName(this, tr("Load Preset"), resolvePresetDir(),
                                                        tr("TOML Files (*.toml)"));
        if (!filePath.isEmpty()) {
            loadPreset(filePath);
        }
    });

    menu->exec(m_btnLoadPreset->mapToGlobal(QPoint(0, m_btnLoadPreset->height())));
}

} // namespace waywal::studio
