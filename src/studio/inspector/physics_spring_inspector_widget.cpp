// SPDX-License-Identifier: GPL-3.0-or-later
#include "physics_spring_inspector_widget.hpp"

#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPointer>
#include <QQueue>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace waywal::studio {

/* ========================================================================= */
/* Helper Functions                                                          */
/* ========================================================================= */

static we_physics_preset_t detectPreset(const we_bone_sim_config_t &cfg)
{
    auto matches = [](float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; };

    if (matches(cfg.compliance_stretch, 1e-4f) && matches(cfg.compliance_bend, 0.001f) &&
        matches(cfg.damping_ratio, 0.05f)) {
        return WE_PHYS_PRESET_SILK;
    }
    if (matches(cfg.compliance_stretch, 5e-5f) && matches(cfg.compliance_bend, 0.005f) &&
        matches(cfg.damping_ratio, 0.10f)) {
        return WE_PHYS_PRESET_LONG_HAIR;
    }
    if (matches(cfg.compliance_stretch, 1e-5f) && matches(cfg.compliance_bend, 0.05f) &&
        matches(cfg.damping_ratio, 0.30f)) {
        return WE_PHYS_PRESET_HEAVY_CAPE;
    }
    if (matches(cfg.compliance_stretch, 1e-6f) && matches(cfg.compliance_bend, 0.50f) &&
        matches(cfg.damping_ratio, 0.40f)) {
        return WE_PHYS_PRESET_FOLIAGE_BRANCH;
    }
    if (matches(cfg.compliance_stretch, 1e-7f) && matches(cfg.compliance_bend, 0.002f) &&
        matches(cfg.damping_ratio, 0.08f)) {
        return WE_PHYS_PRESET_SOFT_RIBBON;
    }
    if (matches(cfg.compliance_stretch, 1e-8f) && matches(cfg.compliance_bend, 0.10f) &&
        matches(cfg.damping_ratio, 0.20f)) {
        return WE_PHYS_PRESET_METALLIC_CHAIN;
    }
    return WE_PHYS_PRESET_CUSTOM;
}

/* ========================================================================= */
/* Construction & Setup                                                      */
/* ========================================================================= */

PhysicsSpringInspectorWidget::PhysicsSpringInspectorWidget(QWidget *parent) : QWidget(parent)
{
    // Initialize default context
    std::memset(&m_ctx, 0, sizeof(m_ctx));
    m_ctx.wind.gravity_vec[0] = 0.0f;
    m_ctx.wind.gravity_vec[1] = -980.0f;
    m_ctx.wind.base_speed_px_s = 200.0f;
    m_ctx.wind.gust_frequency_hz = 0.5f;
    m_ctx.wind.gust_amplitude = 1.0f;
    m_ctx.wind.curl_spatial_scale = 0.002f;

    m_ctx.cursor.radius_px = 120.0f;
    m_ctx.cursor.impulse_strength = 400.0f;
    m_ctx.cursor.falloff_power = 2.0f;

    for (uint32_t i = 0; i < WE_MDL_MAX_BONES; ++i) {
        m_ctx.configs[i].bone_index = i;
        we_physics_apply_preset(&m_ctx.configs[i], WE_PHYS_PRESET_CUSTOM);
    }

    setupUi();
    syncUiFromSelectedBone();
}

void PhysicsSpringInspectorWidget::setupUi()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(6, 6, 6, 6);
    mainLayout->setSpacing(6);

    auto *splitter = new QSplitter(Qt::Vertical, this);
    splitter->setChildrenCollapsible(false);

    // -------------------------------------------------------------------------
    // Top Panel: Skeletal Bone Hierarchy
    // -------------------------------------------------------------------------
    auto *boneTreeGroup = new QGroupBox(tr("Skeletal Bone Hierarchy (XPBD Springs)"), this);
    auto *boneTreeLayout = new QVBoxLayout(boneTreeGroup);
    boneTreeLayout->setContentsMargins(4, 4, 4, 4);
    boneTreeLayout->setSpacing(4);

    m_boneTree = new QTreeWidget(boneTreeGroup);
    m_boneTree->setHeaderLabels({tr("Bone Name"), tr("Physics State")});
    m_boneTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_boneTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_boneTree->setAlternatingRowColors(true);
    m_boneTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_boneTree->setStyleSheet(QStringLiteral(
        "QTreeWidget { background-color: #181825; color: #cdd6f4; border: 1px solid #313244; "
        "border-radius: 4px; }"
        "QTreeWidget::item:selected { background-color: #45475a; color: #89b4fa; }"));

    connect(m_boneTree, &QTreeWidget::currentItemChanged, this,
            &PhysicsSpringInspectorWidget::onBoneTreeCurrentItemChanged);

    boneTreeLayout->addWidget(m_boneTree);
    splitter->addWidget(boneTreeGroup);

    // -------------------------------------------------------------------------
    // Bottom Panel: Scrollable Controls
    // -------------------------------------------------------------------------
    auto *scrollArea = new QScrollArea(splitter);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; }"));

    auto *scrollContent = new QWidget(scrollArea);
    auto *contentLayout = new QVBoxLayout(scrollContent);
    contentLayout->setContentsMargins(0, 0, 4, 0);
    contentLayout->setSpacing(8);

    // --- Section 1: Presets & Actions ---
    auto *presetGroup = new QGroupBox(tr("1-Click Material Presets"), scrollContent);
    auto *presetLayout = new QVBoxLayout(presetGroup);
    presetLayout->setSpacing(6);

    auto *presetRow = new QHBoxLayout();
    auto *presetLbl = new QLabel(tr("Preset:"), presetGroup);
    presetLbl->setStyleSheet(QStringLiteral("color: #bac2de; font-weight: bold;"));
    m_presetCombo = new QComboBox(presetGroup);
    m_presetCombo->addItem(tr("Custom (Manual Parameters)"), WE_PHYS_PRESET_CUSTOM);
    m_presetCombo->addItem(tr("Silk (Ultra-Compliant / Low Damping)"), WE_PHYS_PRESET_SILK);
    m_presetCombo->addItem(tr("Long Hair (Anisotropic Bend / Cascade)"), WE_PHYS_PRESET_LONG_HAIR);
    m_presetCombo->addItem(tr("Heavy Cape (High Mass / Air Drag)"), WE_PHYS_PRESET_HEAVY_CAPE);
    m_presetCombo->addItem(tr("Foliage Branch (High Restoring Stiffness)"),
                           WE_PHYS_PRESET_FOLIAGE_BRANCH);
    m_presetCombo->addItem(tr("Soft Ribbon (Inextensible Cloth Strip)"),
                           WE_PHYS_PRESET_SOFT_RIBBON);
    m_presetCombo->addItem(tr("Metallic Chain (Zero Bend Stiffness / Heavy)"),
                           WE_PHYS_PRESET_METALLIC_CHAIN);
    m_presetCombo->setStyleSheet(
        QStringLiteral("QComboBox { background-color: #313244; color: #cdd6f4; padding: 4px; "
                       "border: 1px solid #45475a; border-radius: 4px; }"));

    connect(m_presetCombo, qOverload<int>(&QComboBox::activated), this,
            &PhysicsSpringInspectorWidget::onPresetComboActivated);

    presetRow->addWidget(presetLbl);
    presetRow->addWidget(m_presetCombo, 1);
    presetLayout->addLayout(presetRow);

    auto *btnRow = new QHBoxLayout();
    m_resetBoneBtn = new QPushButton(tr("Reset Selected Bone"), presetGroup);
    m_resetBoneBtn->setStyleSheet(
        QStringLiteral("QPushButton { background-color: #313244; color: #cdd6f4; padding: 4px 8px; "
                       "border-radius: 4px; }"));
    connect(m_resetBoneBtn, &QPushButton::clicked, this,
            &PhysicsSpringInspectorWidget::onResetBoneClicked);

    m_applyDescendantsBtn = new QPushButton(tr("Cascade to Descendants"), presetGroup);
    m_applyDescendantsBtn->setStyleSheet(
        QStringLiteral("QPushButton { background-color: #313244; color: #89b4fa; padding: 4px 8px; "
                       "border-radius: 4px; }"));
    connect(m_applyDescendantsBtn, &QPushButton::clicked, this,
            &PhysicsSpringInspectorWidget::onApplyPresetToDescendantsClicked);

    btnRow->addWidget(m_resetBoneBtn);
    btnRow->addWidget(m_applyDescendantsBtn);
    presetLayout->addLayout(btnRow);
    contentLayout->addWidget(presetGroup);

    // --- Section 2: XPBD Spring-Bone Parameters ---
    auto *springGroup = new QGroupBox(tr("XPBD Elasticity & Compliance Parameters"), scrollContent);
    auto *springLayout = new QGridLayout(springGroup);
    springLayout->setHorizontalSpacing(6);
    springLayout->setVerticalSpacing(4);

    m_boneEnableCheck = new QCheckBox(tr("Simulate XPBD Dynamics for Selected Bone"), springGroup);
    m_boneEnableCheck->setStyleSheet(
        QStringLiteral("QCheckBox { color: #a6e3a1; font-weight: bold; }"));
    connect(m_boneEnableCheck, &QCheckBox::toggled, this,
            &PhysicsSpringInspectorWidget::onBoneConfigControlChanged);
    springLayout->addWidget(m_boneEnableCheck, 0, 0, 1, 3);

    int row = 1;

    auto addControlRow = [&](const QString &label, const QString &tooltip, double minVal,
                             double maxVal, double step, int decimals, QDoubleSpinBox *&spinOut) {
        auto *lbl = new QLabel(label, springGroup);
        lbl->setToolTip(tooltip);
        lbl->setStyleSheet(QStringLiteral("color: #cdd6f4; font-size: 11px;"));

        auto *slider = new QSlider(Qt::Horizontal, springGroup);
        slider->setRange(0, 1000);
        slider->setMinimumWidth(80);
        slider->setStyleSheet(QStringLiteral(
            "QSlider::groove:horizontal { height: 4px; background: #313244; border-radius: 2px; }"
            "QSlider::sub-page:horizontal { background: #89b4fa; border-radius: 2px; }"
            "QSlider::handle:horizontal { background: #cdd6f4; width: 12px; margin-top: -4px; "
            "margin-bottom: -4px; border-radius: 6px; }"));

        spinOut = new QDoubleSpinBox(springGroup);
        spinOut->setRange(minVal, maxVal);
        spinOut->setSingleStep(step);
        spinOut->setDecimals(decimals);
        spinOut->setFixedWidth(74);
        spinOut->setStyleSheet(
            QStringLiteral("QDoubleSpinBox { background-color: #181825; color: #cdd6f4; border: "
                           "1px solid #313244; border-radius: 3px; padding: 1px 3px; }"));

        if (maxVal > minVal) {
            const int initPos = static_cast<int>(
                std::round(((spinOut->value() - minVal) / (maxVal - minVal)) * 1000.0));
            slider->setValue(std::clamp(initPos, 0, 1000));
        }

        connect(slider, &QSlider::valueChanged, this, [this, spinOut, minVal, maxVal](int val) {
            if (m_updatingUi)
                return;
            double fval = minVal + (static_cast<double>(val) / 1000.0) * (maxVal - minVal);
            spinOut->setValue(fval);
        });

        connect(spinOut, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this, slider, minVal, maxVal](double val) {
                    if (maxVal > minVal) {
                        const int sPos = static_cast<int>(
                            std::round(((val - minVal) / (maxVal - minVal)) * 1000.0));
                        const QSignalBlocker blocker(slider);
                        slider->setValue(std::clamp(sPos, 0, 1000));
                    }
                    if (!m_updatingUi) {
                        onBoneConfigControlChanged();
                    }
                });

        springLayout->addWidget(lbl, row, 0);
        springLayout->addWidget(slider, row, 1);
        springLayout->addWidget(spinOut, row, 2);
        ++row;
    };

    addControlRow(tr("Stretch Compliance alpha_s (m/N):"),
                  tr("XPBD distance constraint compliance. 0.0 = completely rigid rod."), 0.0,
                  0.001, 0.00001, 5, m_stretchComplianceSpin);

    addControlRow(tr("Bend Compliance alpha_b (rad/Nm):"),
                  tr("XPBD angular bending constraint compliance. Lower values resist bending."),
                  0.0, 2.0, 0.001, 3, m_bendComplianceSpin);

    addControlRow(tr("Damping Ratio zeta:"),
                  tr("Rayleigh / XPBD velocity damping. 1.0 = critically damped."), 0.0, 2.0, 0.01,
                  2, m_dampingSpin);

    addControlRow(tr("Particle Mass (kg):"), tr("Effective tip mass in kg."), 0.01, 20.0, 0.05, 2,
                  m_massSpin);

    addControlRow(tr("Gravity Scale:"), tr("Multiplier on scene gravity vector."), -5.0, 5.0, 0.1,
                  1, m_gravityScaleSpin);

    addControlRow(tr("Angle Limit Min (rad):"), tr("Minimum local angular deflection limit."),
                  -3.1416, 0.0, 0.05, 2, m_angleMinSpin);

    addControlRow(tr("Angle Limit Max (rad):"), tr("Maximum local angular deflection limit."), 0.0,
                  3.1416, 0.05, 2, m_angleMaxSpin);

    addControlRow(tr("Capsule Radius (px):"),
                  tr("Capsule radius for cursor SDF repulsion in pixels."), 0.0, 100.0, 1.0, 1,
                  m_collisionRadiusSpin);

    addControlRow(tr("Audio Drag Impulse:"),
                  tr("Coupling gain for PipeWire 64-band Bark audio spectrum impulses."), 0.0, 5.0,
                  0.05, 2, m_audioDragSpin);

    addControlRow(tr("Rayleigh Damping:"), tr("Kelvin-Voigt high-frequency damping factor."), 0.0,
                  0.5, 0.005, 3, m_rayleighDampingSpin);

    contentLayout->addWidget(springGroup);

    // --- Section 3: 3D Simplex Curl-Noise Wind ---
    auto *windGroup = new QGroupBox(tr("3D Simplex Curl-Noise Wind Field"), scrollContent);
    auto *windLayout = new QGridLayout(windGroup);
    windLayout->setHorizontalSpacing(6);
    windLayout->setVerticalSpacing(4);

    m_windEnableCheck = new QCheckBox(tr("Enable Ambient Curl-Noise Wind"), windGroup);
    m_windEnableCheck->setStyleSheet(
        QStringLiteral("QCheckBox { color: #89dceb; font-weight: bold; }"));
    connect(m_windEnableCheck, &QCheckBox::toggled, this,
            &PhysicsSpringInspectorWidget::onWindControlChanged);
    windLayout->addWidget(m_windEnableCheck, 0, 0, 1, 3);

    int wrow = 1;
    auto addWindRow = [&](const QString &label, const QString &tooltip, double minVal,
                          double maxVal, double step, int decimals, QDoubleSpinBox *&spinOut) {
        auto *lbl = new QLabel(label, windGroup);
        lbl->setToolTip(tooltip);
        lbl->setStyleSheet(QStringLiteral("color: #cdd6f4; font-size: 11px;"));

        auto *slider = new QSlider(Qt::Horizontal, windGroup);
        slider->setRange(0, 1000);
        slider->setMinimumWidth(80);
        slider->setStyleSheet(QStringLiteral(
            "QSlider::groove:horizontal { height: 4px; background: #313244; border-radius: 2px; }"
            "QSlider::sub-page:horizontal { background: #89dceb; border-radius: 2px; }"
            "QSlider::handle:horizontal { background: #cdd6f4; width: 12px; margin-top: -4px; "
            "margin-bottom: -4px; border-radius: 6px; }"));

        spinOut = new QDoubleSpinBox(windGroup);
        spinOut->setRange(minVal, maxVal);
        spinOut->setSingleStep(step);
        spinOut->setDecimals(decimals);
        spinOut->setFixedWidth(74);
        spinOut->setStyleSheet(
            QStringLiteral("QDoubleSpinBox { background-color: #181825; color: #cdd6f4; border: "
                           "1px solid #313244; border-radius: 3px; padding: 1px 3px; }"));

        if (maxVal > minVal) {
            const int initPos = static_cast<int>(
                std::round(((spinOut->value() - minVal) / (maxVal - minVal)) * 1000.0));
            slider->setValue(std::clamp(initPos, 0, 1000));
        }

        connect(slider, &QSlider::valueChanged, this, [this, spinOut, minVal, maxVal](int val) {
            if (m_updatingUi)
                return;
            double fval = minVal + (static_cast<double>(val) / 1000.0) * (maxVal - minVal);
            spinOut->setValue(fval);
        });

        connect(spinOut, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this, slider, minVal, maxVal](double val) {
                    if (maxVal > minVal) {
                        const int sPos = static_cast<int>(
                            std::round(((val - minVal) / (maxVal - minVal)) * 1000.0));
                        const QSignalBlocker blocker(slider);
                        slider->setValue(std::clamp(sPos, 0, 1000));
                    }
                    if (!m_updatingUi) {
                        onWindControlChanged();
                    }
                });

        windLayout->addWidget(lbl, wrow, 0);
        windLayout->addWidget(slider, wrow, 1);
        windLayout->addWidget(spinOut, wrow, 2);
        ++wrow;
    };

    addWindRow(tr("Wind Angle (rad):"), tr("Dominant wind direction angle in radians [-pi, pi]."),
               -3.1416, 3.1416, 0.05, 2, m_windDirectionSpin);

    addWindRow(tr("Wind Speed (px/s):"), tr("Mean laminar wind speed in pixels/sec."), 0.0, 2000.0,
               10.0, 0, m_windSpeedSpin);

    addWindRow(tr("Gust Amplitude:"), tr("Peak gust modulation factor [0.0, 4.0]."), 0.0, 4.0, 0.05,
               2, m_windGustSpin);

    addWindRow(tr("Gust Frequency (Hz):"), tr("Temporal frequency of Simplex curl gusts in Hz."),
               0.0, 10.0, 0.1, 1, m_windGustFreqSpin);

    addWindRow(tr("Curl Spatial Scale:"), tr("Spatial frequency 1/L of 2D/3D Simplex potential."),
               0.0001, 0.02, 0.0005, 4, m_windSpatialScaleSpin);

    contentLayout->addWidget(windGroup);

    // --- Section 4: Wayland Cursor SDF Repulsion/Vortex ---
    auto *cursorGroup = new QGroupBox(tr("Wayland Cursor SDF Interaction"), scrollContent);
    auto *cursorLayout = new QGridLayout(cursorGroup);
    cursorLayout->setHorizontalSpacing(6);
    cursorLayout->setVerticalSpacing(4);

    m_cursorEnableCheck = new QCheckBox(tr("Enable Cursor Repulsion / Vortex"), cursorGroup);
    m_cursorEnableCheck->setStyleSheet(
        QStringLiteral("QCheckBox { color: #f9e2af; font-weight: bold; }"));
    connect(m_cursorEnableCheck, &QCheckBox::toggled, this,
            &PhysicsSpringInspectorWidget::onCursorControlChanged);
    cursorLayout->addWidget(m_cursorEnableCheck, 0, 0, 1, 3);

    int crow = 1;
    auto addCursorRow = [&](const QString &label, const QString &tooltip, double minVal,
                            double maxVal, double step, int decimals, QDoubleSpinBox *&spinOut) {
        auto *lbl = new QLabel(label, cursorGroup);
        lbl->setToolTip(tooltip);
        lbl->setStyleSheet(QStringLiteral("color: #cdd6f4; font-size: 11px;"));

        auto *slider = new QSlider(Qt::Horizontal, cursorGroup);
        slider->setRange(0, 1000);
        slider->setMinimumWidth(80);
        slider->setStyleSheet(QStringLiteral(
            "QSlider::groove:horizontal { height: 4px; background: #313244; border-radius: 2px; }"
            "QSlider::sub-page:horizontal { background: #f9e2af; border-radius: 2px; }"
            "QSlider::handle:horizontal { background: #cdd6f4; width: 12px; margin-top: -4px; "
            "margin-bottom: -4px; border-radius: 6px; }"));

        spinOut = new QDoubleSpinBox(cursorGroup);
        spinOut->setRange(minVal, maxVal);
        spinOut->setSingleStep(step);
        spinOut->setDecimals(decimals);
        spinOut->setFixedWidth(74);
        spinOut->setStyleSheet(
            QStringLiteral("QDoubleSpinBox { background-color: #181825; color: #cdd6f4; border: "
                           "1px solid #313244; border-radius: 3px; padding: 1px 3px; }"));

        if (maxVal > minVal) {
            const int initPos = static_cast<int>(
                std::round(((spinOut->value() - minVal) / (maxVal - minVal)) * 1000.0));
            slider->setValue(std::clamp(initPos, 0, 1000));
        }

        connect(slider, &QSlider::valueChanged, this, [this, spinOut, minVal, maxVal](int val) {
            if (m_updatingUi)
                return;
            double fval = minVal + (static_cast<double>(val) / 1000.0) * (maxVal - minVal);
            spinOut->setValue(fval);
        });

        connect(spinOut, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this, slider, minVal, maxVal](double val) {
                    if (maxVal > minVal) {
                        const int sPos = static_cast<int>(
                            std::round(((val - minVal) / (maxVal - minVal)) * 1000.0));
                        const QSignalBlocker blocker(slider);
                        slider->setValue(std::clamp(sPos, 0, 1000));
                    }
                    if (!m_updatingUi) {
                        onCursorControlChanged();
                    }
                });

        cursorLayout->addWidget(lbl, crow, 0);
        cursorLayout->addWidget(slider, crow, 1);
        cursorLayout->addWidget(spinOut, crow, 2);
        ++crow;
    };

    addCursorRow(tr("Influence Radius (px):"), tr("Field influence radius in pixels."), 10.0, 500.0,
                 5.0, 0, m_cursorRadiusSpin);

    addCursorRow(tr("Impulse Strength:"), tr("Force gain (>0 repels, <0 attracts)."), -2000.0,
                 2000.0, 20.0, 0, m_cursorStrengthSpin);

    addCursorRow(tr("Falloff Power:"), tr("Radial falloff exponent p in (1 - d/R)^p."), 0.5, 4.0,
                 0.1, 1, m_cursorFalloffSpin);

    contentLayout->addWidget(cursorGroup);
    contentLayout->addStretch(1);

    scrollArea->setWidget(scrollContent);
    splitter->addWidget(scrollArea);

    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
    mainLayout->addWidget(splitter, 1);

    // -------------------------------------------------------------------------
    // Bottom Status: 240 Hz Solver Telemetry Badge
    // -------------------------------------------------------------------------
    m_telemetryLabel = new QLabel(tr("240 Hz Solver: 0 active bones | 0.0 us / step"), this);
    m_telemetryLabel->setStyleSheet(QStringLiteral(
        "QLabel { background-color: #11111b; color: #a6e3a1; font-family: monospace; font-size: "
        "11px; padding: 4px 8px; border: 1px solid #313244; border-radius: 4px; }"));
    m_telemetryLabel->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(m_telemetryLabel);
}

/* ========================================================================= */
/* Skeleton Loading & Bone Tree Population                                   */
/* ========================================================================= */

void PhysicsSpringInspectorWidget::loadPuppetSkeleton(const we_puppet_t *puppet,
                                                      int32_t sceneObjectId)
{
    m_objectId = sceneObjectId;

    if (!puppet || puppet->bone_count == 0 || !puppet->bones) {
        m_boneNames.clear();
        m_boneParents.clear();
        m_selectedBoneIndex = 0;
        if (m_boneTree) {
            m_boneTree->clear();
        }
        std::memset(&m_ctx, 0, sizeof(m_ctx));
        syncUiFromSelectedBone();
        return;
    }

    m_boneNames.clear();
    m_boneParents.clear();
    m_boneNames.reserve(puppet->bone_count);
    m_boneParents.reserve(puppet->bone_count);

    for (uint32_t i = 0; i < puppet->bone_count && i < WE_MDL_MAX_BONES; ++i) {
        QString name = QString::fromUtf8(puppet->bones[i].name).trimmed();
        if (name.isEmpty()) {
            name = QStringLiteral("Bone %1").arg(i);
        }
        m_boneNames.push_back(name);
        m_boneParents.push_back(puppet->bones[i].parent == 0xFFFFFFFFu
                                    ? -1
                                    : static_cast<int32_t>(puppet->bones[i].parent));
    }

    if (puppet->physics_ctx) {
        m_ctx = *(puppet->physics_ctx);
    } else {
        we_physics_init_from_puppet(&m_ctx, puppet);
    }

    populateBoneList(puppet);

    int32_t firstSelect = 0;
    for (uint32_t i = 0; i < puppet->bone_count && i < WE_MDL_MAX_BONES; ++i) {
        if ((m_ctx.configs[i].flags & WE_BONE_PHYS_ENABLED) != 0) {
            firstSelect = static_cast<int32_t>(i);
            break;
        }
    }
    selectBoneIndex(firstSelect);
}

void PhysicsSpringInspectorWidget::populateBoneList(const we_puppet_t *puppet)
{
    if (!m_boneTree) {
        return;
    }
    m_boneTree->clear();

    uint32_t boneCount = puppet ? puppet->bone_count : static_cast<uint32_t>(m_boneNames.size());
    if (boneCount == 0) {
        return;
    }
    if (boneCount > WE_MDL_MAX_BONES) {
        boneCount = WE_MDL_MAX_BONES;
    }

    std::vector<QTreeWidgetItem *> items(boneCount, nullptr);

    for (uint32_t i = 0; i < boneCount; ++i) {
        auto *item = new QTreeWidgetItem();
        QString name = (i < m_boneNames.size())
                           ? m_boneNames[i]
                           : (puppet ? QString::fromUtf8(puppet->bones[i].name).trimmed()
                                     : QStringLiteral("Bone %1").arg(i));
        if (name.isEmpty()) {
            name = QStringLiteral("Bone %1").arg(i);
        }

        item->setText(0, QStringLiteral("[%1] %2").arg(i).arg(name));
        item->setData(0, Qt::UserRole, static_cast<int>(i));

        bool enabled = (m_ctx.configs[i].flags & WE_BONE_PHYS_ENABLED) != 0;
        item->setText(1, enabled ? QStringLiteral("XPBD Active") : QStringLiteral("Rigid"));
        item->setForeground(1, enabled ? QBrush(QColor(0xa6, 0xe3, 0xa1))
                                       : QBrush(QColor(0x93, 0x99, 0xb2)));
        items[i] = item;
    }

    std::vector<int32_t> assignedParent(boneCount, -1);
    for (uint32_t i = 0; i < boneCount; ++i) {
        int32_t parent = -1;
        if (puppet) {
            parent = (puppet->bones[i].parent == 0xFFFFFFFFu)
                         ? -1
                         : static_cast<int32_t>(puppet->bones[i].parent);
        } else if (i < m_boneParents.size()) {
            parent = m_boneParents[i];
        }

        bool validParent = (parent >= 0 && static_cast<uint32_t>(parent) < boneCount &&
                            parent != static_cast<int32_t>(i) && items[parent] != nullptr);
        if (validParent) {
            for (int32_t anc = parent; anc >= 0; anc = assignedParent[static_cast<size_t>(anc)]) {
                if (anc == static_cast<int32_t>(i)) {
                    validParent = false;
                    break;
                }
            }
        }

        if (validParent) {
            items[parent]->addChild(items[i]);
            assignedParent[i] = parent;
        } else {
            m_boneTree->addTopLevelItem(items[i]);
        }
    }

    m_boneTree->expandAll();
}

/* ========================================================================= */
/* Selection & UI Synchronization                                            */
/* ========================================================================= */

void PhysicsSpringInspectorWidget::selectBoneIndex(int32_t boneIndex)
{
    if (boneIndex < 0 || boneIndex >= static_cast<int32_t>(WE_MDL_MAX_BONES)) {
        return;
    }
    m_selectedBoneIndex = boneIndex;

    if (m_boneTree) {
        QTreeWidgetItemIterator it(m_boneTree);
        while (*it) {
            if ((*it)->data(0, Qt::UserRole).toInt() == boneIndex) {
                m_boneTree->blockSignals(true);
                m_boneTree->setCurrentItem(*it);
                m_boneTree->blockSignals(false);
                break;
            }
            ++it;
        }
    }

    syncUiFromSelectedBone();
}

const we_bone_sim_config_t &PhysicsSpringInspectorWidget::currentBoneConfig() const noexcept
{
    return boneConfig(m_selectedBoneIndex);
}

const we_bone_sim_config_t &
PhysicsSpringInspectorWidget::boneConfig(int32_t boneIndex) const noexcept
{
    if (boneIndex >= 0 && boneIndex < static_cast<int32_t>(WE_MDL_MAX_BONES)) {
        return m_ctx.configs[boneIndex];
    }
    static const we_bone_sim_config_t s_default{};
    return s_default;
}

void PhysicsSpringInspectorWidget::refreshBoneTreeStates()
{
    if (!m_boneTree) {
        return;
    }
    QTreeWidgetItemIterator it(m_boneTree);
    while (*it) {
        const int bIdx = (*it)->data(0, Qt::UserRole).toInt();
        if (bIdx >= 0 && bIdx < static_cast<int32_t>(WE_MDL_MAX_BONES)) {
            const bool enabled = (m_ctx.configs[bIdx].flags & WE_BONE_PHYS_ENABLED) != 0;
            (*it)->setText(1, enabled ? QStringLiteral("XPBD Active") : QStringLiteral("Rigid"));
            (*it)->setForeground(1, enabled ? QBrush(QColor(0xa6, 0xe3, 0xa1))
                                            : QBrush(QColor(0x93, 0x99, 0xb2)));
        }
        ++it;
    }
}

void PhysicsSpringInspectorWidget::syncUiFromSelectedBone()
{
    const QScopedValueRollback<bool> guard(m_updatingUi, true);

    if (m_selectedBoneIndex >= 0 && m_selectedBoneIndex < static_cast<int32_t>(WE_MDL_MAX_BONES)) {
        const we_bone_sim_config_t &cfg = m_ctx.configs[m_selectedBoneIndex];

        if (m_boneEnableCheck) {
            m_boneEnableCheck->setChecked((cfg.flags & WE_BONE_PHYS_ENABLED) != 0);
        }
        if (m_stretchComplianceSpin) {
            m_stretchComplianceSpin->setValue(cfg.compliance_stretch);
        }
        if (m_bendComplianceSpin) {
            m_bendComplianceSpin->setValue(cfg.compliance_bend);
        }
        if (m_dampingSpin) {
            m_dampingSpin->setValue(cfg.damping_ratio);
        }
        if (m_massSpin) {
            m_massSpin->setValue(cfg.mass_kg);
        }
        if (m_gravityScaleSpin) {
            m_gravityScaleSpin->setValue(cfg.gravity_scale);
        }
        if (m_angleMinSpin) {
            m_angleMinSpin->setValue(cfg.angle_limit_min_rad);
        }
        if (m_angleMaxSpin) {
            m_angleMaxSpin->setValue(cfg.angle_limit_max_rad);
        }
        if (m_collisionRadiusSpin) {
            m_collisionRadiusSpin->setValue(cfg.collision_radius_px);
        }
        if (m_audioDragSpin) {
            m_audioDragSpin->setValue(cfg.audio_drag_coeff);
        }
        if (m_rayleighDampingSpin) {
            m_rayleighDampingSpin->setValue(cfg.rayleigh_damping);
        }

        if (m_presetCombo) {
            we_physics_preset_t detected = detectPreset(cfg);
            m_presetCombo->setCurrentIndex(static_cast<int>(detected));
        }
    }

    if (m_windEnableCheck) {
        m_windEnableCheck->setChecked(m_ctx.wind.enabled != 0);
    }
    if (m_windDirectionSpin) {
        m_windDirectionSpin->setValue(m_ctx.wind.base_direction_rad);
    }
    if (m_windSpeedSpin) {
        m_windSpeedSpin->setValue(m_ctx.wind.base_speed_px_s);
    }
    if (m_windGustSpin) {
        m_windGustSpin->setValue(m_ctx.wind.gust_amplitude);
    }
    if (m_windGustFreqSpin) {
        m_windGustFreqSpin->setValue(m_ctx.wind.gust_frequency_hz);
    }
    if (m_windSpatialScaleSpin) {
        m_windSpatialScaleSpin->setValue(m_ctx.wind.curl_spatial_scale);
    }

    if (m_cursorEnableCheck) {
        m_cursorEnableCheck->setChecked(m_ctx.cursor.enabled != 0);
    }
    if (m_cursorRadiusSpin) {
        m_cursorRadiusSpin->setValue(m_ctx.cursor.radius_px);
    }
    if (m_cursorStrengthSpin) {
        m_cursorStrengthSpin->setValue(m_ctx.cursor.impulse_strength);
    }
    if (m_cursorFalloffSpin) {
        m_cursorFalloffSpin->setValue(m_ctx.cursor.falloff_power);
    }
}

/* ========================================================================= */
/* Action Slots & Preset Application                                         */
/* ========================================================================= */

void PhysicsSpringInspectorWidget::applyMaterialPreset(we_physics_preset_t preset)
{
    if (m_selectedBoneIndex < 0 || m_selectedBoneIndex >= static_cast<int32_t>(WE_MDL_MAX_BONES)) {
        return;
    }

    we_bone_sim_config_t newCfg = m_ctx.configs[m_selectedBoneIndex];
    if (preset != WE_PHYS_PRESET_CUSTOM) {
        we_physics_apply_preset(&newCfg, preset);
        newCfg.flags |= WE_BONE_PHYS_ENABLED;
    }

    commitPhysicsChange(newCfg, m_ctx.wind, m_ctx.cursor);
}

void PhysicsSpringInspectorWidget::onBoneTreeCurrentItemChanged(QTreeWidgetItem *current,
                                                                QTreeWidgetItem *previous)
{
    (void)previous;
    if (!current) {
        return;
    }
    int boneIdx = current->data(0, Qt::UserRole).toInt();
    selectBoneIndex(boneIdx);
}

void PhysicsSpringInspectorWidget::onPresetComboActivated(int index)
{
    if (m_updatingUi) {
        return;
    }
    applyMaterialPreset(static_cast<we_physics_preset_t>(index));
}

void PhysicsSpringInspectorWidget::onBoneConfigControlChanged()
{
    if (m_updatingUi || m_selectedBoneIndex < 0 ||
        m_selectedBoneIndex >= static_cast<int32_t>(WE_MDL_MAX_BONES)) {
        return;
    }

    we_bone_sim_config_t newCfg = m_ctx.configs[m_selectedBoneIndex];
    newCfg.bone_index = static_cast<uint32_t>(m_selectedBoneIndex);

    if (m_boneEnableCheck) {
        if (m_boneEnableCheck->isChecked()) {
            newCfg.flags |= WE_BONE_PHYS_ENABLED;
        } else {
            newCfg.flags &= ~WE_BONE_PHYS_ENABLED;
        }
    }

    if (m_stretchComplianceSpin) {
        newCfg.compliance_stretch = static_cast<float>(m_stretchComplianceSpin->value());
    }
    if (m_bendComplianceSpin) {
        newCfg.compliance_bend = static_cast<float>(m_bendComplianceSpin->value());
    }
    if (m_dampingSpin) {
        newCfg.damping_ratio = static_cast<float>(m_dampingSpin->value());
    }
    if (m_massSpin) {
        newCfg.mass_kg = static_cast<float>(m_massSpin->value());
    }
    if (m_gravityScaleSpin) {
        newCfg.gravity_scale = static_cast<float>(m_gravityScaleSpin->value());
    }
    if (m_angleMinSpin) {
        newCfg.angle_limit_min_rad = static_cast<float>(m_angleMinSpin->value());
    }
    if (m_angleMaxSpin) {
        newCfg.angle_limit_max_rad = static_cast<float>(m_angleMaxSpin->value());
    }
    if (m_collisionRadiusSpin) {
        newCfg.collision_radius_px = static_cast<float>(m_collisionRadiusSpin->value());
    }
    if (m_audioDragSpin) {
        newCfg.audio_drag_coeff = static_cast<float>(m_audioDragSpin->value());
    }
    if (m_rayleighDampingSpin) {
        newCfg.rayleigh_damping = static_cast<float>(m_rayleighDampingSpin->value());
    }

    commitPhysicsChange(newCfg, m_ctx.wind, m_ctx.cursor);
}

void PhysicsSpringInspectorWidget::onWindControlChanged()
{
    if (m_updatingUi) {
        return;
    }

    we_wind_field_t newWind = m_ctx.wind;
    if (m_windEnableCheck) {
        newWind.enabled = m_windEnableCheck->isChecked() ? 1u : 0u;
    }
    if (m_windDirectionSpin) {
        newWind.base_direction_rad = static_cast<float>(m_windDirectionSpin->value());
    }
    if (m_windSpeedSpin) {
        newWind.base_speed_px_s = static_cast<float>(m_windSpeedSpin->value());
    }
    if (m_windGustSpin) {
        newWind.gust_amplitude = static_cast<float>(m_windGustSpin->value());
    }
    if (m_windGustFreqSpin) {
        newWind.gust_frequency_hz = static_cast<float>(m_windGustFreqSpin->value());
    }
    if (m_windSpatialScaleSpin) {
        newWind.curl_spatial_scale = static_cast<float>(m_windSpatialScaleSpin->value());
    }

    commitPhysicsChange(m_ctx.configs[m_selectedBoneIndex], newWind, m_ctx.cursor);
}

void PhysicsSpringInspectorWidget::onCursorControlChanged()
{
    if (m_updatingUi) {
        return;
    }

    we_cursor_force_t newCursor = m_ctx.cursor;
    if (m_cursorEnableCheck) {
        newCursor.enabled = m_cursorEnableCheck->isChecked() ? 1u : 0u;
    }
    if (m_cursorRadiusSpin) {
        newCursor.radius_px = static_cast<float>(m_cursorRadiusSpin->value());
    }
    if (m_cursorStrengthSpin) {
        newCursor.impulse_strength = static_cast<float>(m_cursorStrengthSpin->value());
    }
    if (m_cursorFalloffSpin) {
        newCursor.falloff_power = static_cast<float>(m_cursorFalloffSpin->value());
    }

    commitPhysicsChange(m_ctx.configs[m_selectedBoneIndex], m_ctx.wind, newCursor);
}

void PhysicsSpringInspectorWidget::onResetBoneClicked()
{
    if (m_selectedBoneIndex < 0 || m_selectedBoneIndex >= static_cast<int32_t>(WE_MDL_MAX_BONES)) {
        return;
    }
    we_bone_sim_config_t newCfg{};
    newCfg.bone_index = static_cast<uint32_t>(m_selectedBoneIndex);
    we_physics_apply_preset(&newCfg, WE_PHYS_PRESET_CUSTOM);
    newCfg.flags &= ~WE_BONE_PHYS_ENABLED;
    commitPhysicsChange(newCfg, m_ctx.wind, m_ctx.cursor);
}

void PhysicsSpringInspectorWidget::onApplyPresetToDescendantsClicked()
{
    if (m_selectedBoneIndex < 0 || m_selectedBoneIndex >= static_cast<int32_t>(WE_MDL_MAX_BONES)) {
        return;
    }
    const size_t boneLimit = std::min<size_t>(m_boneParents.size(), WE_MDL_MAX_BONES);
    if (boneLimit == 0 || static_cast<size_t>(m_selectedBoneIndex) >= boneLimit) {
        return;
    }

    const we_bone_sim_config_t srcCfg = m_ctx.configs[m_selectedBoneIndex];

    struct BoneConfigDelta {
        int32_t boneIndex{0};
        we_bone_sim_config_t oldCfg{};
        we_bone_sim_config_t newCfg{};
    };
    std::vector<BoneConfigDelta> deltas;

    // Track visited bones so malformed cyclic skeletons terminate in O(N).
    std::vector<uint8_t> visited(boneLimit, 0);
    visited[static_cast<size_t>(m_selectedBoneIndex)] = 1;

    QQueue<int32_t> queue;
    queue.enqueue(m_selectedBoneIndex);

    while (!queue.isEmpty()) {
        const int32_t parent = queue.dequeue();
        for (size_t i = 0; i < boneLimit; ++i) {
            if (m_boneParents[i] == parent && !visited[i]) {
                visited[i] = 1;
                we_bone_sim_config_t childCfg = srcCfg;
                childCfg.bone_index = static_cast<uint32_t>(i);
                deltas.push_back(BoneConfigDelta{
                    static_cast<int32_t>(i),
                    m_ctx.configs[i],
                    childCfg,
                });
                queue.enqueue(static_cast<int32_t>(i));
            }
        }
    }

    if (deltas.empty()) {
        emit statusMessage(
            QStringLiteral("No descendant bones under bone %1").arg(m_selectedBoneIndex));
        return;
    }

    class PropagateBonePhysicsCommand final : public ICommand {
      public:
        PropagateBonePhysicsCommand(QPointer<PhysicsSpringInspectorWidget> widget, QString output,
                                    int32_t objectId, int32_t rootBone,
                                    std::vector<BoneConfigDelta> boneDeltas, we_wind_field_t wind,
                                    we_cursor_force_t cursor)
            : m_widget(std::move(widget)), m_output(std::move(output)), m_objectId(objectId),
              m_rootBone(rootBone), m_deltas(std::move(boneDeltas)), m_wind(wind), m_cursor(cursor)
        {
        }

        void execute() override { apply(true); }
        void undo() override { apply(false); }

        [[nodiscard]] QString describe() const override
        {
            return QStringLiteral("Cascade Physics Preset from Bone %1").arg(m_rootBone);
        }

      private:
        void apply(bool useNew)
        {
            if (!m_widget) {
                return;
            }
            bool selectedModified = false;
            for (const auto &d : m_deltas) {
                if (d.boneIndex >= 0 && d.boneIndex < static_cast<int32_t>(WE_MDL_MAX_BONES)) {
                    m_widget->m_ctx.configs[d.boneIndex] = useNew ? d.newCfg : d.oldCfg;
                    if (d.boneIndex == m_widget->m_selectedBoneIndex) {
                        selectedModified = true;
                    }
                }
            }
            m_widget->refreshBoneTreeStates();
            if (selectedModified && !m_widget->m_updatingUi) {
                m_widget->syncUiFromSelectedBone();
            }
            for (const auto &d : m_deltas) {
                const we_bone_sim_config_t &cfg = useNew ? d.newCfg : d.oldCfg;
                emit m_widget->physicsParamsChanged(m_output, m_objectId, d.boneIndex, cfg, m_wind,
                                                    m_cursor);
                m_widget->sendIpcUpdate(m_output, m_objectId, d.boneIndex, cfg, m_wind, m_cursor);
            }
        }

        QPointer<PhysicsSpringInspectorWidget> m_widget;
        QString m_output;
        int32_t m_objectId{-1};
        int32_t m_rootBone{0};
        std::vector<BoneConfigDelta> m_deltas;
        we_wind_field_t m_wind{};
        we_cursor_force_t m_cursor{};
    };

    auto cmd = std::make_unique<PropagateBonePhysicsCommand>(
        QPointer<PhysicsSpringInspectorWidget>(this), m_targetOutput, m_objectId,
        m_selectedBoneIndex, std::move(deltas), m_ctx.wind, m_ctx.cursor);
    if (m_commandBus) {
        m_commandBus->execute(std::move(cmd));
    } else {
        cmd->execute();
    }

    emit statusMessage(QStringLiteral("Applied physics preset to descendants of bone %1")
                           .arg(m_selectedBoneIndex));
}

/* ========================================================================= */
/* Change Commitment & IPC Transmission                                      */
/* ========================================================================= */

void PhysicsSpringInspectorWidget::commitPhysicsChange(const we_bone_sim_config_t &newCfg,
                                                       const we_wind_field_t &newWind,
                                                       const we_cursor_force_t &newCursor)
{
    if (m_selectedBoneIndex < 0 || m_selectedBoneIndex >= static_cast<int32_t>(WE_MDL_MAX_BONES)) {
        return;
    }

    we_bone_sim_config_t oldCfg = m_ctx.configs[m_selectedBoneIndex];
    we_wind_field_t oldWind = m_ctx.wind;
    we_cursor_force_t oldCursor = m_ctx.cursor;

    if (m_commandBus) {
        QPointer<PhysicsSpringInspectorWidget> self(this);
        auto applyFn = [self](const QString &output, int32_t objectId, int32_t boneIndex,
                              const we_bone_sim_config_t &cfg, const we_wind_field_t &wind,
                              const we_cursor_force_t &cursor) {
            if (!self) {
                return;
            }
            if (boneIndex >= 0 && boneIndex < static_cast<int32_t>(WE_MDL_MAX_BONES)) {
                self->m_ctx.configs[boneIndex] = cfg;
            }
            self->m_ctx.wind = wind;
            self->m_ctx.cursor = cursor;
            self->refreshBoneTreeStates();
            if (boneIndex == self->m_selectedBoneIndex && !self->m_updatingUi) {
                self->syncUiFromSelectedBone();
            }
            emit self->physicsParamsChanged(output, objectId, boneIndex, cfg, wind, cursor);
            self->sendIpcUpdate(output, objectId, boneIndex, cfg, wind, cursor);
        };

        auto cmd = std::make_unique<SetPhysicsSpringCommand>(
            m_targetOutput, m_objectId, m_selectedBoneIndex, oldCfg, newCfg, oldWind, newWind,
            oldCursor, newCursor, std::move(applyFn));
        m_commandBus->execute(std::move(cmd));
    } else {
        m_ctx.configs[m_selectedBoneIndex] = newCfg;
        m_ctx.wind = newWind;
        m_ctx.cursor = newCursor;
        refreshBoneTreeStates();
        syncUiFromSelectedBone();
        emit physicsParamsChanged(m_targetOutput, m_objectId, m_selectedBoneIndex, newCfg, newWind,
                                  newCursor);
        sendIpcUpdate(m_targetOutput, m_objectId, m_selectedBoneIndex, newCfg, newWind, newCursor);
    }

    emit statusMessage(QStringLiteral("XPBD bone %1 parameters updated").arg(m_selectedBoneIndex));
}

template <typename Client = IpcClient>
static void dispatchPhysicsIpc(Client *client, const QString &output, int32_t objectId,
                               int32_t boneIndex, const we_bone_sim_config_t &cfg,
                               const we_wind_field_t &wind, const we_cursor_force_t &cursor)
{
    if constexpr (requires(Client *c) {
                      c->sendSetPhysicsParams(output, objectId, boneIndex, cfg, wind, cursor);
                  }) {
        client->sendSetPhysicsParams(output, objectId, boneIndex, cfg, wind, cursor);
    } else if constexpr (requires(Client *c) {
                             c->sendSetPhysicsParams(output, objectId, boneIndex, &cfg, &wind,
                                                     &cursor);
                         }) {
        client->sendSetPhysicsParams(output, objectId, boneIndex, &cfg, &wind, &cursor);
    } else {
        (void)client;
        (void)output;
        (void)objectId;
        (void)boneIndex;
        (void)cfg;
        (void)wind;
        (void)cursor;
    }
}

void PhysicsSpringInspectorWidget::sendIpcUpdate(const QString &output, int32_t objectId,
                                                 int32_t boneIndex, const we_bone_sim_config_t &cfg,
                                                 const we_wind_field_t &wind,
                                                 const we_cursor_force_t &cursor)
{
    if (!m_ipcClient) {
        return;
    }

    dispatchPhysicsIpc(m_ipcClient, output, objectId, boneIndex, cfg, wind, cursor);
}

void PhysicsSpringInspectorWidget::updateSolverTelemetry(uint32_t activeBones, float stepTimeUs)
{
    if (m_telemetryLabel) {
        m_telemetryLabel->setText(QStringLiteral("240 Hz Solver: %1 active bones | %2 us / step")
                                      .arg(activeBones)
                                      .arg(static_cast<double>(stepTimeUs), 0, 'f', 1));
    }
}

} // namespace waywal::studio
