// SPDX-License-Identifier: GPL-3.0-or-later
#include "property_inspector_widget.hpp"
#include "studio/core/preset_store.hpp"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QPointer>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QSet>
#include <QTextStream>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>

namespace waywal::studio {

namespace {

[[nodiscard]] float sanitizeFiniteFloat(float v, float fallback = 0.0f) noexcept
{
    return std::isfinite(v) ? v : fallback;
}

class SetTextPropertyCommand final : public ICommand {
  public:
    using ApplyFn =
        std::function<void(const QString &output, const QString &name, const QString &text)>;

    SetTextPropertyCommand(QString output, QString name, QString oldText, QString newText,
                           bool coalescible, ApplyFn applyFn)
        : m_output(std::move(output)), m_name(std::move(name)), m_oldText(std::move(oldText)),
          m_newText(std::move(newText)), m_coalescible(coalescible), m_applyFn(std::move(applyFn)),
          m_timestamp(std::chrono::steady_clock::now())
    {
    }

    void execute() override
    {
        if (m_applyFn) {
            m_applyFn(m_output, m_name, m_newText);
        }
    }

    void undo() override
    {
        if (m_applyFn) {
            m_applyFn(m_output, m_name, m_oldText);
        }
    }

    QString describe() const override
    {
        return QStringLiteral("Edit text property '%1'").arg(m_name);
    }

    bool mergeWith(const ICommand *other) override
    {
        if (!m_coalescible) {
            return false;
        }
        const auto *next = dynamic_cast<const SetTextPropertyCommand *>(other);
        if (!next || !next->m_coalescible || next->m_name != m_name || next->m_output != m_output) {
            return false;
        }
        const auto dt =
            std::chrono::duration_cast<std::chrono::milliseconds>(next->m_timestamp - m_timestamp);
        if (dt.count() > 500) {
            return false;
        }
        m_newText = next->m_newText;
        m_timestamp = next->m_timestamp;
        return true;
    }

    [[nodiscard]] size_t memoryFootprint() const override
    {
        return sizeof(*this) + static_cast<size_t>(m_output.size() + m_name.size() +
                                                   m_oldText.size() + m_newText.size()) *
                                   sizeof(char16_t);
    }

  private:
    QString m_output;
    QString m_name;
    QString m_oldText;
    QString m_newText;
    bool m_coalescible{false};
    ApplyFn m_applyFn;
    std::chrono::steady_clock::time_point m_timestamp;
};

class SetShaderComboCommand final : public ICommand {
  public:
    using ApplyFn = std::function<void(const QString &output, int32_t objectId, uint32_t passIndex,
                                       const QString &comboName, int comboIdx, int32_t comboValue)>;

    SetShaderComboCommand(QString output, int32_t objectId, uint32_t passIndex, QString comboName,
                          int oldIdx, int32_t oldVal, int newIdx, int32_t newVal, ApplyFn applyFn)
        : m_output(std::move(output)), m_objectId(objectId), m_passIndex(passIndex),
          m_comboName(std::move(comboName)), m_oldIdx(oldIdx), m_oldVal(oldVal), m_newIdx(newIdx),
          m_newVal(newVal), m_applyFn(std::move(applyFn))
    {
    }

    void execute() override
    {
        if (m_applyFn) {
            m_applyFn(m_output, m_objectId, m_passIndex, m_comboName, m_newIdx, m_newVal);
        }
    }

    void undo() override
    {
        if (m_applyFn) {
            m_applyFn(m_output, m_objectId, m_passIndex, m_comboName, m_oldIdx, m_oldVal);
        }
    }

    QString describe() const override
    {
        return QStringLiteral("Set shader combo '%1'").arg(m_comboName);
    }

    bool mergeWith(const ICommand *other) override
    {
        (void)other;
        return false;
    }

  private:
    QString m_output;
    int32_t m_objectId{-1};
    uint32_t m_passIndex{0};
    QString m_comboName;
    int m_oldIdx{0};
    int32_t m_oldVal{0};
    int m_newIdx{0};
    int32_t m_newVal{0};
    ApplyFn m_applyFn;
};

} // namespace

// =============================================================================
// PropertyRowWidget Implementation
// =============================================================================

PropertyRowWidget::PropertyRowWidget(const InspectorProperty &prop, QWidget *parent)
    : QWidget(parent), m_prop(prop)
{
    if (!m_prop.hasRange) {
        const size_t n = std::max<size_t>(1u, m_prop.numComponents);
        m_prop.minValues.resize(n, 0.0f);
        m_prop.maxValues.resize(n, 1.0f);
        for (size_t i = 0; i < n; ++i) {
            const float defVal = (i < m_prop.defaultValues.size())
                                     ? sanitizeFiniteFloat(m_prop.defaultValues[i])
                                     : 0.0f;
            const float curVal = (i < m_prop.currentValues.size())
                                     ? sanitizeFiniteFloat(m_prop.currentValues[i])
                                     : defVal;
            const float lo = std::min({0.0f, defVal, curVal});
            const float hi = std::max({1.0f, defVal, curVal});
            m_prop.minValues[i] = (lo < 0.0f) ? (lo * 2.0f) : 0.0f;
            m_prop.maxValues[i] = (hi > 1.0f) ? (hi * 2.0f) : 1.0f;
        }
    }
    setupUi();
}

PropertyRowWidget::~PropertyRowWidget() { closeActiveColorDialog(); }

void PropertyRowWidget::closeActiveColorDialog()
{
    if (!m_activeColorDialog) {
        return;
    }
    QPointer<OklchColorDialog> dlg = m_activeColorDialog;
    m_activeColorDialog.clear();
    dlg->disconnect(this);
    dlg->close();
    dlg->deleteLater();
}

void PropertyRowWidget::updateColorSwatchVisual(const QColor &col)
{
    if (!m_colorSwatchBtn) {
        return;
    }
    m_colorSwatchBtn->setStyleSheet(
        QStringLiteral(
            "QPushButton { background-color: %1; border: 1px solid #334155; border-radius: 4px; "
            "min-width: 36px; min-height: 28px; } "
            "QPushButton:focus { border: 2px solid #38BDF8; }")
            .arg(col.name(QColor::HexRgb)));
}

void PropertyRowWidget::componentRange(int index, float &outMin, float &outMax) const noexcept
{
    const float rawMin =
        (index >= 0 && index < static_cast<int>(m_prop.minValues.size()))
            ? sanitizeFiniteFloat(m_prop.minValues[static_cast<size_t>(index)], 0.0f)
            : 0.0f;
    const float defVal =
        (index >= 0 && index < static_cast<int>(m_prop.defaultValues.size()))
            ? sanitizeFiniteFloat(m_prop.defaultValues[static_cast<size_t>(index)], 1.0f)
            : 1.0f;
    const float rawMax =
        (index >= 0 && index < static_cast<int>(m_prop.maxValues.size()))
            ? sanitizeFiniteFloat(m_prop.maxValues[static_cast<size_t>(index)], 1.0f)
            : (defVal > 1.0f ? defVal * 2.0f : 1.0f);

    if (m_prop.hasRange) {
        outMin = rawMin;
        outMax = rawMax;
    } else {
        const float curVal =
            (index >= 0 && index < static_cast<int>(m_prop.currentValues.size()))
                ? sanitizeFiniteFloat(m_prop.currentValues[static_cast<size_t>(index)], defVal)
                : defVal;
        const float lo = std::min({rawMin, 0.0f, defVal, curVal});
        const float hi = std::max({rawMax, 1.0f, defVal, curVal});
        outMin = (lo < 0.0f) ? std::min(rawMin, lo) : 0.0f;
        outMax = std::max(rawMax, hi);
    }

    if (!std::isfinite(outMin)) {
        outMin = 0.0f;
    }
    if (!std::isfinite(outMax) || outMax <= outMin) {
        outMax = outMin + 1.0f;
    }
}

void PropertyRowWidget::setupUi()
{
    auto *rowLayout = new QHBoxLayout(this);
    rowLayout->setContentsMargins(2, 3, 2, 3);
    rowLayout->setSpacing(6);

    const QFontMetrics fm(font());
    const int controlMinHeight = std::max(28, fm.height() + 10);

    // Property Label
    QString displayLabel = !m_prop.label.isEmpty() ? m_prop.label : m_prop.name;
    m_labelWidget = new QLabel(displayLabel, this);
    m_labelWidget->setMinimumWidth(64);
    m_labelWidget->setMaximumWidth(104);
    m_labelWidget->setWordWrap(true);
    m_labelWidget->setStyleSheet("font-size: 11px; color: #F1F5F9;");
    m_labelWidget->setToolTip(
        QString("<b>Uniform:</b> %1<br><b>Category:</b> %2").arg(m_prop.name, m_prop.category));
    rowLayout->addWidget(m_labelWidget);

    auto *controlLayout = new QHBoxLayout();
    controlLayout->setContentsMargins(0, 0, 0, 0);
    controlLayout->setSpacing(6);

    const QString inputCss =
        QStringLiteral("background-color: #12141A; color: #F1F5F9; border: 1px solid #2D3340; "
                       "border-radius: 4px; padding: 2px 6px; font-size: 12px;");
    const QString spinCss = QStringLiteral(
        "QDoubleSpinBox, QSpinBox { background-color: #12141A; color: #F1F5F9; "
        "border: 1px solid #2D3340; border-radius: 4px; padding: 2px 4px; font-size: 12px; } "
        "QDoubleSpinBox:focus, QSpinBox:focus { border: 2px solid #38BDF8; }");

    switch (m_prop.kind) {
    case InspectorProperty::Kind::Float: {
        float minVal = 0.0f;
        float maxVal = 1.0f;
        componentRange(0, minVal, maxVal);

        float stepVal = (std::isfinite(m_prop.step) && m_prop.step > 0.0001f) ? m_prop.step : 0.01f;
        int decimals = 2;
        if (stepVal < 0.001f)
            decimals = 4;
        else if (stepVal < 0.01f)
            decimals = 3;

        auto *slider = new QSlider(Qt::Horizontal, this);
        slider->setRange(0, 1000);
        slider->setMinimumWidth(36);
        slider->setMinimumHeight(controlMinHeight);
        const float fallbackDef =
            !m_prop.defaultValues.empty() ? sanitizeFiniteFloat(m_prop.defaultValues[0]) : 0.0f;
        const float cur = m_prop.currentValues.empty()
                              ? fallbackDef
                              : sanitizeFiniteFloat(m_prop.currentValues[0], fallbackDef);
        const int sliderPos =
            static_cast<int>(std::round(((cur - minVal) / (maxVal - minVal)) * 1000.0f));
        slider->setValue(std::clamp(sliderPos, 0, 1000));

        auto *spin = new QDoubleSpinBox(this);
        if (m_prop.hasRange) {
            spin->setRange(minVal, maxVal);
        } else {
            spin->setRange(-1000000.0, 1000000.0);
        }
        spin->setSingleStep(stepVal);
        spin->setDecimals(decimals);
        spin->setKeyboardTracking(false);
        spin->setValue(cur);
        spin->setMinimumWidth(std::max(64, fm.horizontalAdvance(QStringLiteral("-999.99")) + 22));
        spin->setMinimumHeight(controlMinHeight);
        spin->setStyleSheet(spinCss);

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
        const uint32_t count = std::clamp<uint32_t>(m_prop.numComponents, 1u, 4u);
        const char *compNames[4] = {"X:", "Y:", "Z:", "W:"};

        auto *vecStack = new QVBoxLayout();
        vecStack->setContentsMargins(0, 0, 0, 0);
        vecStack->setSpacing(4);

        const int spinMinW = std::max(60, fm.horizontalAdvance(QStringLiteral("-999.99")) + 20);
        for (uint32_t i = 0; i < count; ++i) {
            float minVal = 0.0f;
            float maxVal = 1.0f;
            componentRange(static_cast<int>(i), minVal, maxVal);
            const float stepVal =
                (std::isfinite(m_prop.step) && m_prop.step > 0.0001f) ? m_prop.step : 0.01f;

            auto *subLayout = new QHBoxLayout();
            subLayout->setContentsMargins(0, 0, 0, 0);
            subLayout->setSpacing(4);
            auto *cLabel = new QLabel(compNames[i], this);
            cLabel->setMinimumWidth(std::max(18, fm.horizontalAdvance(QStringLiteral("W:")) + 2));
            cLabel->setStyleSheet("color: #94A3B8; font-weight: bold; font-size: 11px;");

            auto *slider = new QSlider(Qt::Horizontal, this);
            slider->setRange(0, 1000);
            slider->setMinimumWidth(32);
            slider->setMinimumHeight(controlMinHeight);
            const float fallbackDef = (i < m_prop.defaultValues.size())
                                          ? sanitizeFiniteFloat(m_prop.defaultValues[i])
                                          : 0.0f;
            const float cur = (i < m_prop.currentValues.size())
                                  ? sanitizeFiniteFloat(m_prop.currentValues[i], fallbackDef)
                                  : fallbackDef;
            const int sliderPos =
                static_cast<int>(std::round(((cur - minVal) / (maxVal - minVal)) * 1000.0f));
            slider->setValue(std::clamp(sliderPos, 0, 1000));

            auto *spin = new QDoubleSpinBox(this);
            if (m_prop.hasRange) {
                spin->setRange(minVal, maxVal);
            } else {
                spin->setRange(-1000000.0, 1000000.0);
            }
            spin->setSingleStep(stepVal);
            spin->setDecimals(2);
            spin->setKeyboardTracking(false);
            spin->setValue(cur);
            spin->setMinimumWidth(spinMinW);
            spin->setMinimumHeight(controlMinHeight);
            spin->setStyleSheet(spinCss);

            m_sliders.push_back(slider);
            m_spinBoxes.push_back(spin);

            const int compIdx = static_cast<int>(i);
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
        m_checkBox->setMinimumHeight(controlMinHeight);
        m_checkBox->setStyleSheet(
            "QCheckBox { color: #F1F5F9; font-size: 12px; spacing: 6px; } "
            "QCheckBox:focus { outline: 2px solid #38BDF8; border-radius: 4px; }");
        const bool checked = (!m_prop.currentValues.empty() && m_prop.currentValues[0] > 0.5f);
        m_checkBox->setChecked(checked);
        connect(m_checkBox, &QCheckBox::toggled, this, &PropertyRowWidget::onBoolToggled);
        controlLayout->addWidget(m_checkBox);
        controlLayout->addStretch();
        break;
    }

    case InspectorProperty::Kind::Int: {
        m_intSpinBox = new QSpinBox(this);
        int minVal = -1000000;
        int maxVal = 1000000;
        if (m_prop.hasRange && !m_prop.minValues.empty() && !m_prop.maxValues.empty() &&
            std::isfinite(m_prop.minValues[0]) && std::isfinite(m_prop.maxValues[0]) &&
            m_prop.maxValues[0] > m_prop.minValues[0]) {
            minVal = static_cast<int>(std::lround(m_prop.minValues[0]));
            maxVal = static_cast<int>(std::lround(m_prop.maxValues[0]));
        }
        m_intSpinBox->setRange(minVal, maxVal);
        m_intSpinBox->setKeyboardTracking(false);
        const int defInt =
            !m_prop.defaultValues.empty()
                ? static_cast<int>(std::lround(sanitizeFiniteFloat(m_prop.defaultValues[0])))
                : 0;
        const int cur = !m_prop.currentValues.empty()
                            ? static_cast<int>(std::lround(sanitizeFiniteFloat(
                                  m_prop.currentValues[0], static_cast<float>(defInt))))
                            : defInt;
        m_intSpinBox->setValue(cur);
        m_intSpinBox->setMinimumHeight(controlMinHeight);
        m_intSpinBox->setStyleSheet(spinCss);
        connect(m_intSpinBox, qOverload<int>(&QSpinBox::valueChanged), this,
                &PropertyRowWidget::onIntSpinChanged);
        controlLayout->addWidget(m_intSpinBox);
        controlLayout->addStretch();
        break;
    }

    case InspectorProperty::Kind::Combo: {
        m_comboBox = new QComboBox(this);
        m_comboBox->setMinimumWidth(68);
        m_comboBox->setMinimumHeight(controlMinHeight);
        m_comboBox->setStyleSheet(
            "QComboBox { background-color: #12141A; color: #F1F5F9; border: 1px solid #2D3340; "
            "border-radius: 4px; padding: 2px 6px; font-size: 12px; } "
            "QComboBox:focus { border: 2px solid #38BDF8; }");
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
        if (m_prop.currentValues.size() < 3) {
            m_prop.currentValues.resize(3, 1.0f);
        }
        const float r = std::clamp(sanitizeFiniteFloat(m_prop.currentValues[0], 1.0f), 0.0f, 1.0f);
        const float g = std::clamp(sanitizeFiniteFloat(m_prop.currentValues[1], 1.0f), 0.0f, 1.0f);
        const float b = std::clamp(sanitizeFiniteFloat(m_prop.currentValues[2], 1.0f), 0.0f, 1.0f);
        const QColor curCol = QColor::fromRgbF(r, g, b);

        m_colorSwatchBtn = new QPushButton(this);
        m_colorSwatchBtn->setMinimumSize(36, controlMinHeight);
        m_colorSwatchBtn->setCursor(Qt::PointingHandCursor);
        m_colorSwatchBtn->setToolTip(tr("Click to choose color"));
        updateColorSwatchVisual(curCol);

        m_hexEdit = new QLineEdit(curCol.name(QColor::HexRgb).toUpper(), this);
        m_hexEdit->setMaxLength(7);
        m_hexEdit->setMinimumWidth(
            std::max(72, fm.horizontalAdvance(QStringLiteral("#FFFFFF")) + 16));
        m_hexEdit->setMinimumHeight(controlMinHeight);
        m_hexEdit->setStyleSheet(
            QStringLiteral("QLineEdit { %1 } QLineEdit:focus { border: 2px solid #38BDF8; }")
                .arg(inputCss));

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
        m_textEdit->setMinimumWidth(68);
        m_textEdit->setMinimumHeight(controlMinHeight);
        m_textEdit->setStyleSheet(
            QStringLiteral("QLineEdit { %1 } QLineEdit:focus { border: 2px solid #38BDF8; }")
                .arg(inputCss));
        connect(m_textEdit, &QLineEdit::textEdited, this, &PropertyRowWidget::onTextEdited);
        controlLayout->addWidget(m_textEdit, 1);
        break;
    }
    }

    rowLayout->addLayout(controlLayout, 1);

    const bool supportsAudioBadge = (m_prop.kind == InspectorProperty::Kind::Float ||
                                     m_prop.kind == InspectorProperty::Kind::Vec2 ||
                                     m_prop.kind == InspectorProperty::Kind::Vec3 ||
                                     m_prop.kind == InspectorProperty::Kind::Vec4 ||
                                     m_prop.kind == InspectorProperty::Kind::Color);
    if (supportsAudioBadge) {
        m_audioBadgeButton = new QToolButton(this);
        m_audioBadgeButton->setObjectName(QStringLiteral("AudioBadge_%1").arg(m_prop.name));
        m_audioBadgeButton->setText(tr("Audio"));
        const int badgeW = std::max(48, fm.horizontalAdvance(tr("Audio")) + 14);
        m_audioBadgeButton->setMinimumSize(badgeW, controlMinHeight);
        m_audioBadgeButton->setToolTip(tr("Bind this uniform to live PipeWire Bark audio"));
        setAudioBound(m_audioBound);
        connect(m_audioBadgeButton, &QToolButton::clicked, this, [this]() {
            const float baseline =
                !m_prop.currentValues.empty()
                    ? sanitizeFiniteFloat(m_prop.currentValues[0])
                    : (!m_prop.defaultValues.empty() ? sanitizeFiniteFloat(m_prop.defaultValues[0])
                                                     : 0.0f);
            emit audioBindingRequested(m_prop.name, baseline);
        });
        rowLayout->addWidget(m_audioBadgeButton);
    }

    // Reset-to-default button
    m_resetButton = new QToolButton(this);
    m_resetButton->setText(tr("Reset"));
    const int resetW = std::max(48, fm.horizontalAdvance(tr("Reset")) + 14);
    m_resetButton->setMinimumSize(resetW, controlMinHeight);
    m_resetButton->setToolTip(tr("Reset this parameter to default value"));
    m_resetButton->setStyleSheet(
        "QToolButton { color: #94A3B8; font-size: 11px; background: #20242E; border: 1px solid "
        "#2D3340; border-radius: 4px; padding: 2px 6px; } "
        "QToolButton:hover { color: #F1F5F9; background: #2A303D; } "
        "QToolButton:focus { border: 2px solid #38BDF8; }");
    connect(m_resetButton, &QToolButton::clicked, this, &PropertyRowWidget::resetToDefault);
    rowLayout->addWidget(m_resetButton);
}

void PropertyRowWidget::syncSliderSpin(int index, float val, bool emitSignal)
{
    if (index < 0 || index >= static_cast<int>(m_prop.currentValues.size()))
        return;

    const float finiteVal = sanitizeFiniteFloat(val);
    float minVal = 0.0f;
    float maxVal = 1.0f;
    componentRange(index, minVal, maxVal);

    if (!m_prop.hasRange) {
        // Expand visual slider bounds cleanly when external updates exceed the initial range.
        if (finiteVal < minVal) {
            minVal = (finiteVal < 0.0f) ? finiteVal * 1.5f : finiteVal;
            if (static_cast<size_t>(index) < m_prop.minValues.size()) {
                m_prop.minValues[static_cast<size_t>(index)] = minVal;
            }
        }
        if (finiteVal > maxVal) {
            maxVal = (finiteVal > 0.0f) ? finiteVal * 1.5f : finiteVal + 1.0f;
            if (static_cast<size_t>(index) < m_prop.maxValues.size()) {
                m_prop.maxValues[static_cast<size_t>(index)] = maxVal;
            }
        }
    }

    {
        const QScopedValueRollback<bool> guard(m_updating, true);
        m_prop.currentValues[static_cast<size_t>(index)] = finiteVal;

        if (index < static_cast<int>(m_sliders.size())) {
            const float span = std::max(1e-6f, maxVal - minVal);
            const int sliderPos =
                static_cast<int>(std::round(((finiteVal - minVal) / span) * 1000.0f));
            m_sliders[static_cast<size_t>(index)]->setValue(std::clamp(sliderPos, 0, 1000));
        }
        if (index < static_cast<int>(m_spinBoxes.size())) {
            m_spinBoxes[static_cast<size_t>(index)]->setValue(finiteVal);
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
    if (m_updating || idx < 0)
        return;
    m_prop.comboIndex = idx;
    if (m_prop.currentValues.empty())
        m_prop.currentValues.resize(1);
    m_prop.currentValues[0] = static_cast<float>(idx);
    emitChange();
}

void PropertyRowWidget::onColorButtonClicked()
{
    closeActiveColorDialog();

    const float r = (m_prop.currentValues.size() > 0)
                        ? std::clamp(sanitizeFiniteFloat(m_prop.currentValues[0], 1.0f), 0.0f, 1.0f)
                        : 1.0f;
    const float g = (m_prop.currentValues.size() > 1)
                        ? std::clamp(sanitizeFiniteFloat(m_prop.currentValues[1], 1.0f), 0.0f, 1.0f)
                        : 1.0f;
    const float b = (m_prop.currentValues.size() > 2)
                        ? std::clamp(sanitizeFiniteFloat(m_prop.currentValues[2], 1.0f), 0.0f, 1.0f)
                        : 1.0f;
    const QColor initCol = QColor::fromRgbF(r, g, b);

    auto *dlg = new OklchColorDialog(initCol, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose, true);
    dlg->setModal(false);
    dlg->setWindowTitle(
        tr("Pick Color - %1").arg(m_prop.label.isEmpty() ? m_prop.name : m_prop.label));
    m_activeColorDialog = dlg;

    QPointer<PropertyRowWidget> self(this);
    QPointer<OklchColorDialog> dlgPtr(dlg);

    connect(dlg, &OklchColorDialog::colorChanged, this, [self, dlgPtr](const QColor &newCol) {
        if (!self || !dlgPtr || self->m_activeColorDialog != dlgPtr) {
            return;
        }
        if (self->m_prop.currentValues.size() < 3) {
            self->m_prop.currentValues.resize(3, 1.0f);
        }
        self->m_prop.currentValues[0] = std::clamp(static_cast<float>(newCol.redF()), 0.0f, 1.0f);
        self->m_prop.currentValues[1] = std::clamp(static_cast<float>(newCol.greenF()), 0.0f, 1.0f);
        self->m_prop.currentValues[2] = std::clamp(static_cast<float>(newCol.blueF()), 0.0f, 1.0f);

        self->updateColorSwatchVisual(newCol);
        if (self->m_hexEdit) {
            self->m_hexEdit->setText(newCol.name(QColor::HexRgb).toUpper());
        }
        self->emitChange();
    });

    connect(dlg, &QDialog::accepted, this, [self, dlgPtr]() {
        if (!self || !dlgPtr) {
            return;
        }
        if (self->m_activeColorDialog == dlgPtr) {
            self->m_activeColorDialog.clear();
        }
        const QColor finalCol = dlgPtr->currentColor();
        if (self->m_prop.currentValues.size() < 3) {
            self->m_prop.currentValues.resize(3, 1.0f);
        }
        self->m_prop.currentValues[0] = std::clamp(static_cast<float>(finalCol.redF()), 0.0f, 1.0f);
        self->m_prop.currentValues[1] =
            std::clamp(static_cast<float>(finalCol.greenF()), 0.0f, 1.0f);
        self->m_prop.currentValues[2] =
            std::clamp(static_cast<float>(finalCol.blueF()), 0.0f, 1.0f);
        self->updateColorSwatchVisual(finalCol);
        if (self->m_hexEdit) {
            self->m_hexEdit->setText(finalCol.name(QColor::HexRgb).toUpper());
        }
        self->emitChange();
    });

    connect(dlg, &QDialog::rejected, this, [self, dlgPtr, initCol]() {
        if (!self) {
            return;
        }
        if (self->m_activeColorDialog == dlgPtr) {
            self->m_activeColorDialog.clear();
        }
        if (self->m_prop.currentValues.size() < 3) {
            self->m_prop.currentValues.resize(3, 1.0f);
        }
        self->m_prop.currentValues[0] = std::clamp(static_cast<float>(initCol.redF()), 0.0f, 1.0f);
        self->m_prop.currentValues[1] =
            std::clamp(static_cast<float>(initCol.greenF()), 0.0f, 1.0f);
        self->m_prop.currentValues[2] = std::clamp(static_cast<float>(initCol.blueF()), 0.0f, 1.0f);
        self->updateColorSwatchVisual(initCol);
        if (self->m_hexEdit) {
            self->m_hexEdit->setText(initCol.name(QColor::HexRgb).toUpper());
        }
        self->emitChange();
    });

    dlg->show();
}

void PropertyRowWidget::onHexEdited(const QString &text)
{
    if (m_updating)
        return;
    QString cleaned = text.trimmed();
    if (!cleaned.startsWith('#'))
        cleaned.prepend('#');
    if (QColor::isValidColorName(cleaned)) {
        const QColor col(cleaned);
        if (m_prop.currentValues.size() < 3)
            m_prop.currentValues.resize(3, 1.0f);
        m_prop.currentValues[0] = std::clamp(static_cast<float>(col.redF()), 0.0f, 1.0f);
        m_prop.currentValues[1] = std::clamp(static_cast<float>(col.greenF()), 0.0f, 1.0f);
        m_prop.currentValues[2] = std::clamp(static_cast<float>(col.blueF()), 0.0f, 1.0f);

        updateColorSwatchVisual(col);
        emitChange();
    }
}

void PropertyRowWidget::onTextEdited(const QString &text)
{
    if (m_updating)
        return;
    m_prop.textValue = text;
    m_nextTextEditCoalescible = true;
    emit textValueChanged(m_prop.name, text);
    m_nextTextEditCoalescible = false;
}

void PropertyRowWidget::emitChange()
{
    emit valueChanged(m_prop.name, m_prop.currentValues, m_prop.numComponents);
}

void PropertyRowWidget::updateValues(const std::vector<float> &vals, bool emitSignal)
{
    if (m_prop.kind == InspectorProperty::Kind::Text) {
        return;
    }

    std::vector<float> sanitized = vals;
    for (size_t i = 0; i < sanitized.size(); ++i) {
        const float def =
            (i < m_prop.defaultValues.size()) ? sanitizeFiniteFloat(m_prop.defaultValues[i]) : 0.0f;
        sanitized[i] = sanitizeFiniteFloat(sanitized[i], def);
    }

    {
        const QScopedValueRollback<bool> guard(m_updating, true);
        m_prop.currentValues = sanitized;

        if (m_prop.kind == InspectorProperty::Kind::Float && !sanitized.empty()) {
            syncSliderSpin(0, sanitized[0], false);
        } else if (m_prop.kind == InspectorProperty::Kind::Bool && !sanitized.empty()) {
            if (m_checkBox)
                m_checkBox->setChecked(sanitized[0] > 0.5f);
        } else if (m_prop.kind == InspectorProperty::Kind::Int && !sanitized.empty()) {
            if (m_intSpinBox)
                m_intSpinBox->setValue(static_cast<int>(std::lround(sanitized[0])));
        } else if (m_prop.kind == InspectorProperty::Kind::Combo && !sanitized.empty()) {
            const int idx = static_cast<int>(std::lround(sanitized[0]));
            if (m_comboBox && idx >= 0 && idx < m_comboBox->count()) {
                m_prop.comboIndex = idx;
                m_comboBox->setCurrentIndex(idx);
            }
        } else if (m_prop.kind == InspectorProperty::Kind::Color && sanitized.size() >= 3) {
            const QColor col = QColor::fromRgbF(std::clamp(sanitized[0], 0.0f, 1.0f),
                                                std::clamp(sanitized[1], 0.0f, 1.0f),
                                                std::clamp(sanitized[2], 0.0f, 1.0f));
            updateColorSwatchVisual(col);
            if (m_hexEdit) {
                m_hexEdit->setText(col.name(QColor::HexRgb).toUpper());
            }
        } else {
            for (size_t i = 0; i < sanitized.size() && i < m_sliders.size(); ++i) {
                syncSliderSpin(static_cast<int>(i), sanitized[i], false);
            }
        }
    }

    if (emitSignal) {
        emitChange();
    }
}

void PropertyRowWidget::updateTextValue(const QString &text, bool emitSignal)
{
    if (m_prop.kind != InspectorProperty::Kind::Text) {
        return;
    }

    {
        const QScopedValueRollback<bool> guard(m_updating, true);
        m_prop.textValue = text;
        if (m_textEdit && m_textEdit->text() != text) {
            m_textEdit->setText(text);
        }
    }

    m_nextTextEditCoalescible = false;
    if (emitSignal) {
        emit textValueChanged(m_prop.name, text);
    }
}

void PropertyRowWidget::resetToDefault()
{
    if (m_prop.kind == InspectorProperty::Kind::Text) {
        updateTextValue(m_prop.defaultTextValue, true);
        return;
    }
    updateValues(m_prop.defaultValues);
}

void PropertyRowWidget::setFilterVisible(bool visible) { setVisible(visible); }

void PropertyRowWidget::setAudioBound(bool bound)
{
    m_audioBound = bound;
    if (!m_audioBadgeButton) {
        return;
    }
    if (bound) {
        m_audioBadgeButton->setStyleSheet(
            "QToolButton { color: #0F172A; font-size: 11px; font-weight: bold; background: "
            "#38BDF8; border: 1px solid #38BDF8; border-radius: 4px; padding: 2px 6px; } "
            "QToolButton:hover { background: #7DD3FC; } "
            "QToolButton:focus { border: 2px solid #F1F5F9; }");
    } else {
        m_audioBadgeButton->setStyleSheet(
            "QToolButton { color: #94A3B8; font-size: 11px; background: #181B22; border: 1px "
            "solid #2D3340; border-radius: 4px; padding: 2px 6px; } "
            "QToolButton:hover { color: #38BDF8; border-color: #38BDF8; } "
            "QToolButton:focus { border: 2px solid #38BDF8; }");
    }
}

// =============================================================================
// PropertyInspectorWidget Implementation
// =============================================================================

PropertyInspectorWidget::PropertyInspectorWidget(QWidget *parent) : QWidget(parent)
{
    setupUi();
    populateDefaultSceneParameters();
}

PropertyInspectorWidget::~PropertyInspectorWidget() { closeActivePresetPopups(); }

void PropertyInspectorWidget::setupUi()
{
    setStyleSheet(
        "PropertyInspectorWidget { background-color: #12141A; color: #F1F5F9; font-size: 12px; } "
        "QLineEdit { background-color: #12141A; color: #F1F5F9; border: 1px solid #2D3340; "
        "border-radius: 4px; padding: 3px 8px; font-size: 12px; } "
        "QLineEdit:focus { border: 2px solid #38BDF8; } "
        "QPushButton { background-color: #20242E; color: #F1F5F9; border: 1px solid #2D3340; "
        "padding: 4px 10px; border-radius: 5px; font-size: 12px; } "
        "QPushButton:hover { background-color: #2A303D; } "
        "QPushButton:focus { border: 2px solid #38BDF8; }");

    const QFontMetrics fm(font());
    const int btnMinH = std::max(28, fm.height() + 10);

    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(10, 8, 10, 8);
    rootLayout->setSpacing(8);

    // --- Header Card ---
    auto *headerWidget = new QWidget(this);
    headerWidget->setStyleSheet(
        "background: #181B22; border: 1px solid #2D3340; border-radius: 6px;");
    auto *headerLayout = new QVBoxLayout(headerWidget);
    headerLayout->setContentsMargins(10, 8, 10, 8);
    headerLayout->setSpacing(4);

    auto *topRow = new QHBoxLayout();
    topRow->setSpacing(8);
    m_titleLabel = new QLabel(m_currentTitle, headerWidget);
    m_titleLabel->setMinimumWidth(60);
    m_titleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_titleLabel->setStyleSheet("font-size: 13px; font-weight: bold; color: #F1F5F9;");

    m_typeBadge = new QLabel(m_currentType, headerWidget);
    m_typeBadge->setStyleSheet("background: #0284C7; color: #F8FAFC; font-weight: bold; "
                               "border-radius: 4px; padding: 2px 8px; font-size: 11px;");

    topRow->addWidget(m_titleLabel, 1);
    topRow->addWidget(m_typeBadge, 0);
    headerLayout->addLayout(topRow);

    m_authorLabel = new QLabel(headerWidget);
    m_authorLabel->setMinimumWidth(50);
    m_authorLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_authorLabel->setStyleSheet("color: #94A3B8; font-size: 11px;");
    m_authorLabel->setVisible(false);
    headerLayout->addWidget(m_authorLabel);

    rootLayout->addWidget(headerWidget);

    // --- Toolbar: Search Filter & Presets ---
    m_filterEdit = new QLineEdit(this);
    m_filterEdit->setPlaceholderText(tr("Filter parameters..."));
    m_filterEdit->setClearButtonEnabled(true);
    m_filterEdit->setMinimumHeight(btnMinH);
    connect(m_filterEdit, &QLineEdit::textChanged, this,
            &PropertyInspectorWidget::onFilterTextChanged);
    rootLayout->addWidget(m_filterEdit);

    auto *toolBar = new QHBoxLayout();
    toolBar->setContentsMargins(0, 0, 0, 0);
    toolBar->setSpacing(8);

    m_btnSavePreset = new QPushButton(tr("Save"), this);
    m_btnSavePreset->setMinimumSize(std::max(56, fm.horizontalAdvance(tr("Save")) + 20), btnMinH);
    m_btnSavePreset->setToolTip(tr("Save current parameters as a named preset"));
    connect(m_btnSavePreset, &QPushButton::clicked, this,
            &PropertyInspectorWidget::onSavePresetClicked);
    toolBar->addWidget(m_btnSavePreset, 1);

    m_btnLoadPreset = new QPushButton(tr("Load"), this);
    m_btnLoadPreset->setMinimumSize(std::max(56, fm.horizontalAdvance(tr("Load")) + 20), btnMinH);
    m_btnLoadPreset->setToolTip(tr("Load parameters from a saved preset"));
    connect(m_btnLoadPreset, &QPushButton::clicked, this,
            &PropertyInspectorWidget::onLoadPresetClicked);
    toolBar->addWidget(m_btnLoadPreset, 1);

    m_btnResetAll = new QPushButton(tr("Reset"), this);
    m_btnResetAll->setMinimumSize(std::max(56, fm.horizontalAdvance(tr("Reset")) + 20), btnMinH);
    m_btnResetAll->setToolTip(tr("Reset all parameters to their default values"));
    m_btnResetAll->setStyleSheet(
        "QPushButton { background: #20242E; color: #FCA5A5; border: 1px solid #2D3340; "
        "padding: 4px 10px; border-radius: 5px; font-size: 12px; } "
        "QPushButton:hover { background: #2A303D; } "
        "QPushButton:focus { border: 2px solid #38BDF8; }");
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
    m_categorySections.clear();
    m_emptyFilterLabel = nullptr;

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
    m_typeBadge->setStyleSheet("background: #A6D189; color: #0F172A; font-weight: bold; "
                               "border-radius: 4px; padding: 2px 8px; font-size: 11px;");

    for (uint32_t i = 0; i < proj.prop_count && i < WE_MAX_USER_PROPS; ++i) {
        const we_user_prop_t &p = proj.props[i];
        InspectorProperty prop;
        prop.id = QString::fromUtf8(p.key);
        prop.name = QString::fromUtf8(p.key);
        prop.label = QString::fromUtf8(p.text);
        if (prop.label.isEmpty())
            prop.label = prop.name;
        prop.category = deduceCategory(prop.name, prop.label);
        prop.numComponents =
            std::clamp<uint32_t>(p.num_components > 0 ? p.num_components : 1u, 1u, 4u);

        prop.currentValues.resize(prop.numComponents);
        prop.defaultValues.resize(prop.numComponents);
        prop.minValues.resize(prop.numComponents, 0.0f);
        prop.maxValues.resize(prop.numComponents, 1.0f);

        for (uint32_t c = 0; c < prop.numComponents; ++c) {
            const float finiteVal = sanitizeFiniteFloat(p.val[c]);
            prop.currentValues[c] = finiteVal;
            prop.defaultValues[c] = finiteVal;
        }

        switch (p.type) {
        case WE_PROP_FLOAT: {
            prop.kind = InspectorProperty::Kind::Float;
            const float def = prop.defaultValues[0];
            prop.minValues[0] = (def < 0.0f) ? std::min(-1.0f, def * 2.0f) : 0.0f;
            prop.maxValues[0] = (def > 1.0f) ? std::max(10.0f, def * 2.0f) : 1.0f;
            break;
        }
        case WE_PROP_BOOL:
            prop.kind = InspectorProperty::Kind::Bool;
            break;
        case WE_PROP_INT: {
            prop.kind = InspectorProperty::Kind::Int;
            const float def = prop.defaultValues[0];
            prop.minValues[0] = (def < 0.0f) ? std::min(-10.0f, def * 2.0f) : 0.0f;
            prop.maxValues[0] = (def > 0.0f) ? std::max(100.0f, def * 2.0f) : 100.0f;
            break;
        }
        case WE_PROP_COLOR:
            prop.kind = InspectorProperty::Kind::Color;
            prop.numComponents = 3;
            prop.currentValues.resize(3, 1.0f);
            prop.defaultValues.resize(3, 1.0f);
            prop.minValues.resize(3, 0.0f);
            prop.maxValues.resize(3, 1.0f);
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
    setShaderMetadata(meta, QString());
}

void PropertyInspectorWidget::setShaderMetadata(const waywal_shader_meta_t &meta,
                                                const QString &rawComboJson)
{
    QString nextTitle = QString::fromUtf8(meta.title);
    if (nextTitle.isEmpty())
        nextTitle = tr("GLSL Shader");

    const QString nextId = nextTitle.toLower().replace(QRegularExpression("[^a-z0-9_]+"), "_");
    const QString nextType =
        meta.has_multi_pass ? QStringLiteral("MULTI-PASS GLSL") : QStringLiteral("COMPUTE GLSL");
    const QString author = QString::fromUtf8(meta.author);

    std::vector<InspectorProperty> nextProps;
    nextProps.reserve(std::min<uint32_t>(meta.uniform_count, WAYWAL_MAX_SHADER_UNIFORMS));

    for (uint32_t i = 0; i < meta.uniform_count && i < WAYWAL_MAX_SHADER_UNIFORMS; ++i) {
        const waywal_shader_uniform_def_t &u = meta.uniforms[i];
        InspectorProperty prop;
        prop.id = QString::fromUtf8(u.name);
        prop.name = QString::fromUtf8(u.name);
        prop.label = QString::fromUtf8(u.label);
        if (prop.label.isEmpty())
            prop.label = prop.name;
        prop.category = deduceCategory(prop.name, prop.label);
        prop.numComponents =
            std::clamp<uint32_t>(u.num_components > 0 ? u.num_components : 1u, 1u, 4u);
        prop.step = (std::isfinite(u.step) && u.step > 0.0001f) ? u.step : 0.01f;
        prop.hasRange = u.has_range;

        prop.currentValues.resize(prop.numComponents);
        prop.defaultValues.resize(prop.numComponents);
        prop.minValues.resize(prop.numComponents);
        prop.maxValues.resize(prop.numComponents);

        for (uint32_t c = 0; c < prop.numComponents; ++c) {
            const float def = sanitizeFiniteFloat(u.default_val[c]);
            prop.currentValues[c] = def;
            prop.defaultValues[c] = def;
            prop.minValues[c] = sanitizeFiniteFloat(u.min_val[c], def < 0.0f ? def * 2.0f : 0.0f);
            prop.maxValues[c] = sanitizeFiniteFloat(u.max_val[c], def > 1.0f ? def * 2.0f : 1.0f);
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
            prop.numComponents = 3;
            prop.currentValues.resize(3, 1.0f);
            prop.defaultValues.resize(3, 1.0f);
            prop.minValues.resize(3, 0.0f);
            prop.maxValues.resize(3, 1.0f);
            break;
        }

        nextProps.push_back(std::move(prop));
    }

    if (!rawComboJson.isEmpty()) {
        const QJsonDocument comboDoc = QJsonDocument::fromJson(rawComboJson.toUtf8());
        QJsonArray comboArr;
        if (comboDoc.isArray()) {
            comboArr = comboDoc.array();
        } else if (comboDoc.isObject()) {
            comboArr = comboDoc.object().value(QStringLiteral("combos")).toArray();
        }

        for (const QJsonValue &val : comboArr) {
            if (!val.isObject()) {
                continue;
            }
            const QJsonObject obj = val.toObject();
            QString comboName = obj.value(QStringLiteral("name")).toString();
            if (comboName.isEmpty()) {
                comboName = obj.value(QStringLiteral("combo")).toString();
            }
            if (comboName.isEmpty()) {
                continue;
            }

            const int32_t comboDefault = obj.value(QStringLiteral("default"))
                                             .toInt(obj.value(QStringLiteral("value")).toInt(0));

            InspectorProperty prop;
            prop.id = comboName;
            prop.name = comboName;
            prop.label = obj.value(QStringLiteral("label")).toString(comboName);
            prop.category = QStringLiteral("Shader Combos");
            prop.kind = InspectorProperty::Kind::Combo;
            prop.numComponents = 1;
            prop.isShaderCombo = true;

            const QJsonValue optsVal = obj.value(QStringLiteral("options"));
            if (optsVal.isObject()) {
                const QJsonObject optsObj = optsVal.toObject();
                for (auto it = optsObj.constBegin(); it != optsObj.constEnd(); ++it) {
                    const int32_t v =
                        it.value().toInt(static_cast<int32_t>(prop.comboValues.size()));
                    prop.comboOptions.append(QStringLiteral("%1 (%2)").arg(it.key()).arg(v));
                    prop.comboValues.push_back(v);
                }
            } else if (optsVal.isArray()) {
                const QJsonArray optsArr = optsVal.toArray();
                for (qsizetype idx = 0; idx < optsArr.size(); ++idx) {
                    const QJsonValue &optItem = optsArr[idx];
                    if (optItem.isObject()) {
                        const QJsonObject optObj = optItem.toObject();
                        const int32_t v =
                            optObj.value(QStringLiteral("value")).toInt(static_cast<int32_t>(idx));
                        QString lbl = optObj.value(QStringLiteral("label")).toString();
                        if (lbl.isEmpty()) {
                            lbl = optObj.value(QStringLiteral("name")).toString();
                        }
                        if (lbl.isEmpty()) {
                            lbl = QString::number(v);
                        }
                        prop.comboOptions.append(QStringLiteral("%1 (%2)").arg(lbl).arg(v));
                        prop.comboValues.push_back(v);
                    } else {
                        prop.comboOptions.append(optItem.toString());
                        prop.comboValues.push_back(static_cast<int32_t>(idx));
                    }
                }
            }
            if (prop.comboOptions.isEmpty()) {
                const int32_t maxVal = std::max(1, comboDefault);
                for (int32_t v = 0; v <= maxVal; ++v) {
                    prop.comboOptions.append(
                        v == 0 ? QStringLiteral("0 (Off)")
                               : (v == 1 ? QStringLiteral("1 (On)") : QString::number(v)));
                    prop.comboValues.push_back(v);
                }
            }

            int defaultIdx = 0;
            for (size_t idx = 0; idx < prop.comboValues.size(); ++idx) {
                if (prop.comboValues[idx] == comboDefault) {
                    defaultIdx = static_cast<int>(idx);
                    break;
                }
            }
            prop.comboIndex = defaultIdx;
            prop.currentValues = {static_cast<float>(defaultIdx)};
            prop.defaultValues = {static_cast<float>(defaultIdx)};
            prop.minValues = {0.0f};
            prop.maxValues = {
                static_cast<float>(std::max<qsizetype>(1, prop.comboOptions.size() - 1))};
            nextProps.push_back(std::move(prop));
        }
    }

    auto sameSchema = [](const InspectorProperty &a, const InspectorProperty &b) {
        return a.id == b.id && a.name == b.name && a.kind == b.kind &&
               a.numComponents == b.numComponents && a.hasRange == b.hasRange &&
               a.minValues == b.minValues && a.maxValues == b.maxValues &&
               std::abs(a.step - b.step) < 1e-6f && a.isShaderCombo == b.isShaderCombo &&
               a.comboOptions == b.comboOptions && a.comboValues == b.comboValues &&
               a.defaultTextValue == b.defaultTextValue;
    };

    if (m_currentId == nextId && m_currentType == nextType &&
        m_properties.size() == nextProps.size() &&
        std::ranges::equal(m_properties, nextProps, sameSchema)) {
        m_currentTitle = nextTitle;
        m_titleLabel->setText(m_currentTitle);
        if (!author.isEmpty()) {
            m_authorLabel->setText(tr("Author: %1").arg(author));
            m_authorLabel->setVisible(true);
        } else {
            m_authorLabel->clear();
            m_authorLabel->setVisible(false);
        }
        for (size_t i = 0; i < m_properties.size(); ++i) {
            m_properties[i].label = nextProps[i].label;
            m_properties[i].category = nextProps[i].category;
            m_properties[i].defaultValues = std::move(nextProps[i].defaultValues);
        }
        return;
    }

    const bool sameShader = (m_currentId == nextId && m_currentType == nextType);
    QSet<QString> preservedAudio;
    QHash<QString, std::vector<float>> preservedValues;
    QHash<QString, QString> preservedTextValues;
    if (sameShader) {
        for (const auto &existing : m_properties) {
            preservedValues.insert(existing.name, existing.currentValues);
            if (existing.kind == InspectorProperty::Kind::Text) {
                preservedTextValues.insert(existing.name, existing.textValue);
            }
        }
    }
    for (auto it = m_rowWidgets.constBegin(); it != m_rowWidgets.constEnd(); ++it) {
        if (it.value() && it.value()->isAudioBound()) {
            preservedAudio.insert(it.key());
        }
    }
    const int prevScroll = (m_scrollArea && m_scrollArea->verticalScrollBar())
                               ? m_scrollArea->verticalScrollBar()->value()
                               : 0;

    clear();

    m_currentTitle = nextTitle;
    m_currentId = nextId;
    m_currentType = nextType;

    m_titleLabel->setText(m_currentTitle);
    m_typeBadge->setText(m_currentType);
    m_typeBadge->setStyleSheet("background: #EF9F76; color: #0F172A; font-weight: bold; "
                               "border-radius: 4px; padding: 2px 8px; font-size: 11px;");

    if (!author.isEmpty()) {
        m_authorLabel->setText(tr("Author: %1").arg(author));
        m_authorLabel->setVisible(true);
    }

    if (sameShader) {
        for (auto &prop : nextProps) {
            if (preservedValues.contains(prop.name)) {
                const auto &oldVals = preservedValues.value(prop.name);
                if (oldVals.size() == prop.currentValues.size()) {
                    prop.currentValues = oldVals;
                    if (prop.kind == InspectorProperty::Kind::Combo && !oldVals.empty()) {
                        prop.comboIndex = static_cast<int>(std::lround(oldVals[0]));
                    }
                }
            }
            if (prop.kind == InspectorProperty::Kind::Text &&
                preservedTextValues.contains(prop.name)) {
                prop.textValue = preservedTextValues.value(prop.name);
            }
        }
    }
    m_properties = std::move(nextProps);

    rebuildPropertyUi();

    for (const QString &name : preservedAudio) {
        if (auto *row = m_rowWidgets.value(name, nullptr)) {
            row->setAudioBound(true);
        }
    }
    if (m_scrollArea && m_scrollArea->verticalScrollBar()) {
        m_scrollArea->verticalScrollBar()->setValue(prevScroll);
    }
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
        QJsonArray comboArr;
        QFile f(shaderPath);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream ts(&f);
            while (!ts.atEnd()) {
                const QString line = ts.readLine().trimmed();
                const qsizetype tagPos =
                    line.indexOf(QStringLiteral("[COMBO]"), 0, Qt::CaseInsensitive);
                if (tagPos < 0) {
                    continue;
                }
                const qsizetype bracePos = line.indexOf(QLatin1Char('{'), tagPos);
                const qsizetype endBrace = line.lastIndexOf(QLatin1Char('}'));
                if (bracePos >= 0 && endBrace > bracePos) {
                    const QJsonDocument doc = QJsonDocument::fromJson(
                        line.mid(bracePos, endBrace - bracePos + 1).toUtf8());
                    if (doc.isObject()) {
                        comboArr.append(doc.object());
                    }
                }
            }
        }
        const QString comboJson =
            comboArr.isEmpty()
                ? QString()
                : QString::fromUtf8(QJsonDocument(comboArr).toJson(QJsonDocument::Compact));
        setShaderMetadata(meta, comboJson);
        if (meta.title[0] == '\0') {
            const QString fileBase = QFileInfo(shaderPath).completeBaseName();
            if (!fileBase.isEmpty()) {
                m_currentId = PresetStore::sanitizeId(fileBase);
            }
        }
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
    m_categorySections.clear();
    m_emptyFilterLabel = nullptr;

    if (m_properties.empty()) {
        auto *emptyLabel = new QLabel(tr("No editable parameters found."), m_containerWidget);
        emptyLabel->setAlignment(Qt::AlignCenter);
        emptyLabel->setStyleSheet("color: #94A3B8; padding: 24px; font-size: 13px;");
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
            "QGroupBox { font-weight: bold; font-size: 11px; border: 1px solid #2D3340; "
            "border-radius: 6px; margin-top: 8px; padding-top: 10px; background: #181B22; "
            "color: #F1F5F9; } "
            "QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; "
            "left: 10px; top: 0px; padding: 0px 6px 2px 6px; color: #38BDF8; }");

        auto *groupLayout = new QVBoxLayout(group);
        groupLayout->setContentsMargins(8, 6, 8, 6);
        groupLayout->setSpacing(6);

        QList<PropertyRowWidget *> sectionRows;
        for (const auto &p : propList) {
            auto *row = new PropertyRowWidget(p, group);
            connect(row, &PropertyRowWidget::valueChanged, this,
                    &PropertyInspectorWidget::onRowValueChanged);
            connect(row, &PropertyRowWidget::textValueChanged, this,
                    &PropertyInspectorWidget::onRowTextValueChanged);
            connect(row, &PropertyRowWidget::audioBindingRequested, this,
                    &PropertyInspectorWidget::audioBindingRequested);
            m_rowWidgets[p.name] = row;
            sectionRows.append(row);
            groupLayout->addWidget(row);
        }

        m_categorySections.append({group, sectionRows});
        m_containerLayout->addWidget(group);
    }

    m_emptyFilterLabel = new QLabel(tr("No matching parameters found."), m_containerWidget);
    m_emptyFilterLabel->setAlignment(Qt::AlignCenter);
    m_emptyFilterLabel->setStyleSheet("color: #94A3B8; padding: 20px; font-size: 12px;");
    m_emptyFilterLabel->setVisible(false);
    m_containerLayout->addWidget(m_emptyFilterLabel);

    m_containerLayout->addStretch(1);
    applyFilter();
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

QString PropertyInspectorWidget::propertyTextValue(const QString &name) const
{
    for (const auto &prop : m_properties) {
        if (prop.name == name) {
            return prop.textValue;
        }
    }
    return {};
}

void PropertyInspectorWidget::applyShaderComboChange(const QString &output, int32_t objectId,
                                                     uint32_t passIndex, const QString &name,
                                                     int comboIdx, int32_t comboValue)
{
    for (auto &prop : m_properties) {
        if (prop.name == name) {
            prop.comboIndex = comboIdx;
            prop.currentValues = {static_cast<float>(comboIdx)};
            break;
        }
    }
    if (auto it = m_rowWidgets.find(name); it != m_rowWidgets.end() && *it) {
        (*it)->updateValues({static_cast<float>(comboIdx)}, false);
    }
    emit shaderComboChanged(output, objectId, passIndex, name, comboValue);
    emit uniformDefaultEdited(name, QString::number(comboValue));
}

void PropertyInspectorWidget::onRowValueChanged(const QString &name,
                                                const std::vector<float> &values, uint32_t count)
{
    std::vector<float> oldValues;
    bool isShaderCombo = false;
    int oldComboIdx = 0;
    int newComboIdx = 0;
    int32_t oldComboValue = 0;
    int32_t newComboValue = 0;
    InspectorProperty::Kind propKind = InspectorProperty::Kind::Float;

    for (auto &prop : m_properties) {
        if (prop.name == name) {
            propKind = prop.kind;
            if (propKind == InspectorProperty::Kind::Text) {
                return;
            }
            oldValues = prop.currentValues;
            isShaderCombo = prop.isShaderCombo;
            if (isShaderCombo && !values.empty()) {
                oldComboIdx = prop.comboIndex;
                newComboIdx = static_cast<int>(std::lround(values[0]));
                if (oldComboIdx >= 0 && oldComboIdx < static_cast<int>(prop.comboValues.size())) {
                    oldComboValue = prop.comboValues[static_cast<size_t>(oldComboIdx)];
                } else {
                    oldComboValue = oldComboIdx;
                }
                if (newComboIdx >= 0 && newComboIdx < static_cast<int>(prop.comboValues.size())) {
                    newComboValue = prop.comboValues[static_cast<size_t>(newComboIdx)];
                } else {
                    newComboValue = newComboIdx;
                }
            }
            prop.currentValues = values;
            break;
        }
    }

    if (isShaderCombo) {
        if (m_commandBus) {
            QPointer<PropertyInspectorWidget> self(this);
            auto cmd = std::make_unique<SetShaderComboCommand>(
                m_targetOutput, m_targetObjectId, m_targetPassIndex, name, oldComboIdx,
                oldComboValue, newComboIdx, newComboValue,
                [self](const QString &out, int32_t objId, uint32_t passIdx, const QString &nm,
                       int cIdx, int32_t cVal) {
                    if (!self) {
                        return;
                    }
                    self->applyShaderComboChange(out, objId, passIdx, nm, cIdx, cVal);
                });
            m_commandBus->execute(std::move(cmd));
        } else {
            applyShaderComboChange(m_targetOutput, m_targetObjectId, m_targetPassIndex, name,
                                   newComboIdx, newComboValue);
        }
        return;
    }

    dispatchUniform(name, oldValues, values, count);

    if (!values.empty()) {
        if (propKind == InspectorProperty::Kind::Bool) {
            emit uniformDefaultEdited(name, values[0] > 0.5f ? QStringLiteral("true")
                                                             : QStringLiteral("false"));
        } else if (propKind == InspectorProperty::Kind::Int) {
            emit uniformDefaultEdited(name, QString::number(std::lround(values[0])));
        } else if (values.size() == 1 || count == 1) {
            emit uniformDefaultEdited(name, QString::number(values[0], 'f', 3));
        } else {
            QStringList parts;
            const size_t n = std::min<size_t>(values.size(), std::max<uint32_t>(1u, count));
            for (size_t i = 0; i < n; ++i) {
                parts.append(QString::number(values[i], 'f', 3));
            }
            emit uniformDefaultEdited(name, parts.join(QLatin1Char(' ')));
        }
    }
}

void PropertyInspectorWidget::onRowTextValueChanged(const QString &name, const QString &text)
{
    QString oldText;
    bool found = false;
    for (auto &prop : m_properties) {
        if (prop.name == name && prop.kind == InspectorProperty::Kind::Text) {
            oldText = prop.textValue;
            prop.textValue = text;
            found = true;
            break;
        }
    }
    if (!found) {
        return;
    }

    if (m_commandBus) {
        bool coalescible = false;
        if (auto *row = qobject_cast<PropertyRowWidget *>(sender())) {
            coalescible = row->consumeTextEditCoalescible();
        }
        QPointer<PropertyInspectorWidget> self(this);
        auto cmd = std::make_unique<SetTextPropertyCommand>(
            m_targetOutput, name, oldText, text, coalescible,
            [self](const QString &, const QString &nm, const QString &txt) {
                if (!self) {
                    return;
                }
                for (auto &prop : self->m_properties) {
                    if (prop.name == nm && prop.kind == InspectorProperty::Kind::Text) {
                        prop.textValue = txt;
                        break;
                    }
                }
                if (auto it = self->m_rowWidgets.find(nm); it != self->m_rowWidgets.end() && *it) {
                    (*it)->updateTextValue(txt, false);
                }
                emit self->uniformDefaultEdited(nm, txt);
            });
        m_commandBus->execute(std::move(cmd));
    } else {
        emit uniformDefaultEdited(name, text);
    }
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

void PropertyInspectorWidget::applyFilter()
{
    const QString query = m_filterEdit ? m_filterEdit->text().trimmed().toLower() : QString();
    bool anySectionVisible = false;

    for (const auto &section : m_categorySections) {
        QGroupBox *group = section.first;
        bool anyRowVisible = false;
        for (PropertyRowWidget *row : section.second) {
            if (!row) {
                continue;
            }
            const InspectorProperty &p = row->property();
            const bool match =
                query.isEmpty() || p.name.toLower().contains(query) ||
                p.label.toLower().contains(query) || p.category.toLower().contains(query) ||
                (p.kind == InspectorProperty::Kind::Text && p.textValue.toLower().contains(query));
            row->setFilterVisible(match);
            if (match) {
                anyRowVisible = true;
            }
        }
        if (group) {
            group->setVisible(anyRowVisible);
        }
        if (anyRowVisible) {
            anySectionVisible = true;
        }
    }

    if (m_emptyFilterLabel) {
        m_emptyFilterLabel->setVisible(!m_properties.empty() && !anySectionVisible);
    }
}

void PropertyInspectorWidget::onFilterTextChanged(const QString &text)
{
    (void)text;
    applyFilter();
}

void PropertyInspectorWidget::closeActivePresetPopups()
{
    if (m_activePresetMenu) {
        QPointer<QMenu> menu = m_activePresetMenu;
        m_activePresetMenu.clear();
        menu->disconnect(this);
        menu->close();
        menu->deleteLater();
    }
    if (m_savePresetDialog) {
        QPointer<QInputDialog> dlg = m_savePresetDialog;
        m_savePresetDialog.clear();
        dlg->disconnect(this);
        dlg->close();
        dlg->deleteLater();
    }
    if (m_loadPresetDialog) {
        QPointer<QFileDialog> dlg = m_loadPresetDialog;
        m_loadPresetDialog.clear();
        dlg->disconnect(this);
        dlg->close();
        dlg->deleteLater();
    }
}

QString PropertyInspectorWidget::resolvePresetDir() const
{
    QString dir;
    if (m_presetStore) {
        dir = m_presetStore->presetDirectory();
    } else {
        PresetStore fallbackStore(QString(), nullptr);
        dir = fallbackStore.presetDirectory();
    }
    if (!dir.isEmpty()) {
        QDir().mkpath(dir);
    }
    return dir;
}

bool PropertyInspectorWidget::savePreset(const QString &presetName)
{
    const QString trimmedName = presetName.trimmed();
    if (trimmedName.isEmpty()) {
        openSavePresetDialog();
        return false;
    }

    PresetStore *store = m_presetStore;
    PresetStore fallbackStore(QString(), nullptr);
    if (!store) {
        store = &fallbackStore;
    }

    const QString safeId = PresetStore::sanitizeId(trimmedName);
    WallpaperPreset preset{};
    if (store->hasPreset(safeId)) {
        (void)store->loadPreset(safeId, preset);
    }

    preset.id = safeId;
    if (preset.title.isEmpty()) {
        preset.title = m_currentTitle.isEmpty() ? trimmedName : m_currentTitle;
    }
    preset.extraProperties[QStringLiteral("target_project")] = m_currentId;

    for (const auto &p : m_properties) {
        auto valAt = [&p](size_t idx, float fallback = 0.0f) -> float {
            if (idx < p.currentValues.size()) {
                return sanitizeFiniteFloat(p.currentValues[idx], fallback);
            }
            if (idx < p.defaultValues.size()) {
                return sanitizeFiniteFloat(p.defaultValues[idx], fallback);
            }
            return fallback;
        };

        if (p.kind == InspectorProperty::Kind::Color) {
            preset.uniforms[p.name] = UniformValue::fromColor(QColor::fromRgbF(
                std::clamp(valAt(0, 1.0f), 0.0f, 1.0f), std::clamp(valAt(1, 1.0f), 0.0f, 1.0f),
                std::clamp(valAt(2, 1.0f), 0.0f, 1.0f),
                p.currentValues.size() >= 4 ? std::clamp(valAt(3, 1.0f), 0.0f, 1.0f) : 1.0f));
        } else if (p.kind == InspectorProperty::Kind::Vec2) {
            preset.uniforms[p.name] = UniformValue::fromVec2(valAt(0), valAt(1));
        } else if (p.kind == InspectorProperty::Kind::Vec3) {
            preset.uniforms[p.name] = UniformValue::fromVec3(valAt(0), valAt(1), valAt(2));
        } else if (p.kind == InspectorProperty::Kind::Vec4) {
            preset.uniforms[p.name] =
                UniformValue::fromVec4(valAt(0), valAt(1), valAt(2), valAt(3));
        } else if (p.kind == InspectorProperty::Kind::Bool) {
            preset.uniforms[p.name] = UniformValue::fromBool(valAt(0) > 0.5f);
        } else if (p.kind == InspectorProperty::Kind::Int ||
                   p.kind == InspectorProperty::Kind::Combo) {
            preset.uniforms[p.name] =
                UniformValue::fromInt(static_cast<int32_t>(std::lround(valAt(0))));
        } else if (p.kind == InspectorProperty::Kind::Text) {
            preset.uniforms[p.name] = UniformValue::fromString(p.textValue);
        } else {
            preset.uniforms[p.name] = UniformValue::fromFloat(valAt(0));
        }
    }

    if (!store->savePreset(preset)) {
        emit statusMessage(tr("Failed to save preset: %1").arg(safeId));
        return false;
    }

    const QString filePath = store->resolvePresetFilePath(safeId);
    emit presetSaved(safeId, filePath);
    emit statusMessage(tr("Preset saved to %1").arg(filePath));
    return true;
}

bool PropertyInspectorWidget::loadPreset(const QString &presetPathOrName)
{
    const QString trimmed = presetPathOrName.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }

    WallpaperPreset loaded{};
    QString presetId;
    QString filePath;

    if (QDir::isAbsolutePath(trimmed)) {
        const QFileInfo fi(trimmed);
        if (!fi.exists() || !fi.isFile() || fi.isSymLink() ||
            fi.suffix().compare(QStringLiteral("toml"), Qt::CaseInsensitive) != 0) {
            emit statusMessage(tr("Failed to load preset: invalid path %1").arg(trimmed));
            return false;
        }
        presetId = fi.completeBaseName();
        filePath = fi.absoluteFilePath();
        PresetStore customStore(fi.absolutePath(), nullptr);
        if (!customStore.loadPreset(presetId, loaded)) {
            emit statusMessage(tr("Failed to load preset: %1").arg(filePath));
            return false;
        }
    } else {
        if (trimmed.contains(QLatin1Char('/')) || trimmed.contains(QLatin1Char('\\')) ||
            trimmed.contains(QStringLiteral(".."))) {
            emit statusMessage(tr("Rejected unsafe preset identifier: %1").arg(trimmed));
            return false;
        }
        QString baseName = trimmed;
        if (baseName.endsWith(QStringLiteral(".toml"), Qt::CaseInsensitive)) {
            baseName.chop(5);
        }
        presetId = PresetStore::sanitizeId(baseName);

        PresetStore *store = m_presetStore;
        PresetStore fallbackStore(QString(), nullptr);
        if (!store) {
            store = &fallbackStore;
        }
        filePath = store->resolvePresetFilePath(presetId);
        if (!store->loadPreset(presetId, loaded)) {
            emit statusMessage(tr("Failed to load preset: file not found %1").arg(filePath));
            return false;
        }
    }

    for (const auto &[key, uval] : loaded.uniforms) {
        PropertyRowWidget *row = m_rowWidgets.value(key, nullptr);
        if (!row) {
            continue;
        }
        const InspectorProperty &prop = row->property();
        if (prop.kind == InspectorProperty::Kind::Text) {
            if (uval.type == UniformValue::Type::String) {
                row->updateTextValue(uval.stringVal, true);
            }
            continue;
        }

        std::vector<float> parsedVals;
        switch (uval.type) {
        case UniformValue::Type::Float:
            parsedVals.push_back(sanitizeFiniteFloat(uval.floats[0]));
            break;
        case UniformValue::Type::Int:
            parsedVals.push_back(static_cast<float>(uval.intVal));
            break;
        case UniformValue::Type::Bool:
            parsedVals.push_back(uval.boolVal ? 1.0f : 0.0f);
            break;
        case UniformValue::Type::Vec2:
            parsedVals = {sanitizeFiniteFloat(uval.floats[0]), sanitizeFiniteFloat(uval.floats[1])};
            break;
        case UniformValue::Type::Vec3:
            parsedVals = {sanitizeFiniteFloat(uval.floats[0]), sanitizeFiniteFloat(uval.floats[1]),
                          sanitizeFiniteFloat(uval.floats[2])};
            break;
        case UniformValue::Type::Vec4:
            if (prop.kind == InspectorProperty::Kind::Color) {
                parsedVals = {std::clamp(sanitizeFiniteFloat(uval.floats[0]), 0.0f, 1.0f),
                              std::clamp(sanitizeFiniteFloat(uval.floats[1]), 0.0f, 1.0f),
                              std::clamp(sanitizeFiniteFloat(uval.floats[2]), 0.0f, 1.0f)};
            } else {
                parsedVals = {
                    sanitizeFiniteFloat(uval.floats[0]), sanitizeFiniteFloat(uval.floats[1]),
                    sanitizeFiniteFloat(uval.floats[2]), sanitizeFiniteFloat(uval.floats[3])};
            }
            break;
        case UniformValue::Type::Color:
            parsedVals = {std::clamp(sanitizeFiniteFloat(uval.floats[0]), 0.0f, 1.0f),
                          std::clamp(sanitizeFiniteFloat(uval.floats[1]), 0.0f, 1.0f),
                          std::clamp(sanitizeFiniteFloat(uval.floats[2]), 0.0f, 1.0f)};
            break;
        case UniformValue::Type::String:
            break;
        }

        if (!parsedVals.empty()) {
            row->updateValues(parsedVals, true);
        }
    }

    emit presetLoaded(presetId, filePath);
    const QString targetProject = loaded.extraProperties.contains(QStringLiteral("target_project"))
                                      ? loaded.extraProperties.at(QStringLiteral("target_project"))
                                      : QString();
    if (!targetProject.isEmpty() && m_currentId != QStringLiteral("default") &&
        targetProject != m_currentId) {
        emit statusMessage(tr("Preset loaded: %1 (from %2)").arg(presetId, targetProject));
    } else {
        emit statusMessage(tr("Preset loaded: %1").arg(presetId));
    }
    return true;
}

void PropertyInspectorWidget::resetAllToDefaults()
{
    for (auto it = m_rowWidgets.begin(); it != m_rowWidgets.end(); ++it) {
        if (it.value()) {
            it.value()->resetToDefault();
        }
    }
    emit resetToDefaultsTriggered();
    emit statusMessage(tr("All parameters reset to default values."));
}

QStringList PropertyInspectorWidget::listAvailablePresets() const
{
    if (m_presetStore) {
        return m_presetStore->listPresetIds();
    }
    PresetStore fallbackStore(QString(), nullptr);
    return fallbackStore.listPresetIds();
}

void PropertyInspectorWidget::openSavePresetDialog()
{
    if (m_savePresetDialog) {
        QPointer<QInputDialog> prev = m_savePresetDialog;
        m_savePresetDialog.clear();
        prev->disconnect(this);
        prev->close();
        prev->deleteLater();
    }

    auto *dlg = new QInputDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose, true);
    dlg->setModal(false);
    dlg->setWindowTitle(tr("Save Preset"));
    dlg->setLabelText(tr("Enter a name for this preset:"));
    dlg->setInputMode(QInputDialog::TextInput);
    dlg->setTextValue(m_currentId + QStringLiteral("_preset"));
    m_savePresetDialog = dlg;

    QPointer<PropertyInspectorWidget> self(this);
    QPointer<QInputDialog> dlgPtr(dlg);
    connect(dlg, &QInputDialog::textValueSelected, this, [self, dlgPtr](const QString &name) {
        if (!self) {
            return;
        }
        if (self->m_savePresetDialog == dlgPtr) {
            self->m_savePresetDialog.clear();
        }
        if (!name.trimmed().isEmpty()) {
            self->savePreset(name);
        }
    });
    connect(dlg, &QDialog::rejected, this, [self, dlgPtr]() {
        if (self && self->m_savePresetDialog == dlgPtr) {
            self->m_savePresetDialog.clear();
        }
    });

    dlg->show();
}

void PropertyInspectorWidget::openLoadPresetBrowseDialog()
{
    if (m_loadPresetDialog) {
        QPointer<QFileDialog> prev = m_loadPresetDialog;
        m_loadPresetDialog.clear();
        prev->disconnect(this);
        prev->close();
        prev->deleteLater();
    }

    auto *dlg =
        new QFileDialog(this, tr("Load Preset"), resolvePresetDir(), tr("TOML Files (*.toml)"));
    dlg->setFileMode(QFileDialog::ExistingFile);
    dlg->setAcceptMode(QFileDialog::AcceptOpen);
    dlg->setAttribute(Qt::WA_DeleteOnClose, true);
    dlg->setModal(false);
    m_loadPresetDialog = dlg;

    QPointer<PropertyInspectorWidget> self(this);
    QPointer<QFileDialog> dlgPtr(dlg);
    connect(dlg, &QFileDialog::fileSelected, this, [self, dlgPtr](const QString &filePath) {
        if (!self) {
            return;
        }
        if (self->m_loadPresetDialog == dlgPtr) {
            self->m_loadPresetDialog.clear();
        }
        if (!filePath.isEmpty()) {
            self->loadPreset(filePath);
        }
    });
    connect(dlg, &QDialog::rejected, this, [self, dlgPtr]() {
        if (self && self->m_loadPresetDialog == dlgPtr) {
            self->m_loadPresetDialog.clear();
        }
    });

    dlg->show();
}

void PropertyInspectorWidget::onSavePresetClicked() { openSavePresetDialog(); }

void PropertyInspectorWidget::onLoadPresetClicked()
{
    if (m_activePresetMenu) {
        QPointer<QMenu> prev = m_activePresetMenu;
        m_activePresetMenu.clear();
        prev->disconnect(this);
        prev->close();
        prev->deleteLater();
    }

    const QStringList presets = listAvailablePresets();
    if (presets.isEmpty()) {
        openLoadPresetBrowseDialog();
        return;
    }

    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose, true);
    m_activePresetMenu = menu;

    QPointer<PropertyInspectorWidget> self(this);
    for (const QString &presetId : presets) {
        auto *action = menu->addAction(presetId);
        connect(action, &QAction::triggered, this, [self, presetId]() {
            if (self) {
                self->loadPreset(presetId);
            }
        });
    }
    menu->addSeparator();
    auto *browseAction = menu->addAction(tr("Browse other..."));
    connect(browseAction, &QAction::triggered, this, [self]() {
        if (self) {
            self->openLoadPresetBrowseDialog();
        }
    });

    const QPoint anchor = m_btnLoadPreset
                              ? m_btnLoadPreset->mapToGlobal(QPoint(0, m_btnLoadPreset->height()))
                              : mapToGlobal(QPoint(0, 0));
    menu->popup(anchor);
}

} // namespace waywal::studio
