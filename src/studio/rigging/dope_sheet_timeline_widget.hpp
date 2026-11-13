// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_RIGGING_DOPE_SHEET_TIMELINE_WIDGET_HPP
#define WAYWAL_STUDIO_RIGGING_DOPE_SHEET_TIMELINE_WIDGET_HPP

#include "studio/core/command_bus.hpp"
#include "studio/core/ipc_client.hpp"
#include "studio/core/rigging_commands.hpp"
#include "waywal/we_mesh_rig.h"
#include "waywal/we_puppet.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QString>
#include <QTimer>
#include <QTreeWidget>
#include <QWidget>

#include <cstdint>
#include <vector>

namespace waywal::studio {

enum class TimelineViewMode : uint8_t {
    DopeSheet = 0,
    FCurve = 1,
    Split = 2,
};

enum class KeyTangentMode : uint8_t {
    Auto = 0,
    Aligned = 1,
    Vector = 2,
    Stepped = 3,
};

struct TimelineFCurveChannel {
    int32_t boneIndex{-1};
    uint32_t channelIndex{0}; /* 0..8 */
    QString label;
    bool visible{true};
    std::vector<FCurveKeySnapshot> keys;
};

struct TimelineAnimClip {
    int32_t id{100};
    QString name;
    we_mdl_anim_mode_t mode{WE_ANIM_LOOP};
    we_anim_blend_mode_t blendMode{WE_ANIM_BLEND_OVERRIDE};
    float fps{30.0f};
    int32_t lengthFrames{120};
    float layerRate{1.0f};
    float layerBlend{1.0f};
    bool layerVisible{true};
    std::vector<TimelineFCurveChannel> channels;
};

class DopeSheetTimelineWidget : public QWidget {
    Q_OBJECT

  public:
    explicit DopeSheetTimelineWidget(QWidget *parent = nullptr);
    ~DopeSheetTimelineWidget() override;

    void setCommandBus(CommandBus *bus) noexcept { m_commandBus = bus; }
    [[nodiscard]] CommandBus *commandBus() const noexcept { return m_commandBus; }

    void setIpcClient(IpcClient *client) noexcept { m_ipcClient = client; }
    [[nodiscard]] IpcClient *ipcClient() const noexcept { return m_ipcClient; }

    void loadPuppetAnimations(const we_puppet_t *puppet, int32_t objectId);
    void setCurrentFrame(double frame);
    void setPlaying(bool playing);
    void setViewMode(TimelineViewMode mode);
    void setOnionSkinEnabled(bool enabled);

    [[nodiscard]] double currentFrame() const noexcept { return m_currentFrame; }
    [[nodiscard]] bool isPlaying() const noexcept { return m_isPlaying; }
    [[nodiscard]] bool isTimerActive() const noexcept { return m_playTimer.isActive(); }
    [[nodiscard]] TimelineViewMode viewMode() const noexcept { return m_viewMode; }
    [[nodiscard]] const std::vector<TimelineAnimClip> &clips() const noexcept { return m_clips; }

    /* Evaluates channel F-curve at frame using Fritsch-Carlson / cubic Bezier solver */
    [[nodiscard]] static double evaluateChannelAtFrame(const TimelineFCurveChannel &channel,
                                                       double frame);

    /* Bakes sparse F-curves into uniform we_mdl_keyframe_t tracks for MDLA / IPC */
    void bakeClipToTracks(uint32_t clipIndex, uint32_t boneCount,
                          std::vector<we_mdl_bone_track_t> &outTracks,
                          std::vector<we_mdl_keyframe_t> &outStorage) const;

  public slots:
    void onPlayPauseToggled();
    void onStepFrame(int deltaFrames);
    void onInsertKeyframeAtPlayhead();
    void onSetSelectedTangentMode(KeyTangentMode mode);
    void onBakeAndSyncTracks();
    void applyUndoKeySnapshot(uint32_t animId, const std::vector<FCurveKeySnapshot> &keys);

  signals:
    void frameChanged(double frame, double timeSec);
    void onionSkinChanged(bool enabled, int beforeFrames, int afterFrames, float opacity);
    void statusMessage(const QString &msg);

  protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void changeEvent(QEvent *event) override;

  private slots:
    void onPlayTick();

  private:
    void setupUi();
    void updatePlaybackTimerState();

    int32_t m_objectId{-1};
    uint32_t m_boneCount{0};
    uint32_t m_activeClipIndex{0};
    double m_currentFrame{0.0};
    double m_zoomPxPerFrame{12.0};
    bool m_isPlaying{false};
    bool m_windowMinimized{false};
    bool m_scrubbing{false};
    bool m_onionEnabled{false};
    TimelineViewMode m_viewMode{TimelineViewMode::Split};
    std::vector<TimelineAnimClip> m_clips;

    QTimer m_playTimer;
    QElapsedTimer m_frameClock;

    CommandBus *m_commandBus{nullptr};
    IpcClient *m_ipcClient{nullptr};

    QComboBox *m_clipCombo{nullptr};
    QComboBox *m_viewModeCombo{nullptr};
    QComboBox *m_animModeCombo{nullptr};
    QDoubleSpinBox *m_fpsSpin{nullptr};
    QSpinBox *m_lengthSpin{nullptr};
    QDoubleSpinBox *m_layerBlendSpin{nullptr};
    QDoubleSpinBox *m_layerRateSpin{nullptr};
    QCheckBox *m_onionCheck{nullptr};
    QPushButton *m_playBtn{nullptr};
    QLabel *m_timecodeLabel{nullptr};
    QTreeWidget *m_trackTree{nullptr};
    QSplitter *m_mainSplitter{nullptr};
    QWidget *m_dopeCanvas{nullptr};
    QWidget *m_fcurveCanvas{nullptr};
};

} // namespace waywal::studio

#endif // WAYWAL_STUDIO_RIGGING_DOPE_SHEET_TIMELINE_WIDGET_HPP
