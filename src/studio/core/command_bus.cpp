// SPDX-License-Identifier: GPL-3.0-or-later
#include "command_bus.hpp"
#include "command_ring_arena.hpp"

#include <QFileInfo>
#include <QPointer>
#include <QStringList>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <exception>
#include <typeinfo>

namespace waywal::studio {

namespace {

[[nodiscard]] bool allFiniteVec(const std::vector<float> &vals) noexcept
{
    const size_t n = vals.size();
    const float *p = vals.data();
    for (size_t i = 0; i < n; ++i) {
        if (!std::isfinite(p[i])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool qStringsEqualFast(const QString &a, const QString &b) noexcept
{
    const qsizetype n = a.size();
    if (n != b.size()) {
        return false;
    }
    return n == 0 || a.constData() == b.constData() || a == b;
}

[[nodiscard]] bool allFiniteArr4(const std::array<float, 4> &vals) noexcept
{
    for (float v : vals) {
        if (!std::isfinite(v)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool isValidBezierCurve(const bezier_curve_t &curve) noexcept
{
    return std::isfinite(curve.p1x) && std::isfinite(curve.p1y) && std::isfinite(curve.p2x) &&
           std::isfinite(curve.p2y) && curve.p1x >= 0.0f && curve.p1x <= 1.0f &&
           curve.p2x >= 0.0f && curve.p2x <= 1.0f;
}

[[nodiscard]] size_t qStringHeapBytes(const QString &s) noexcept
{
    return static_cast<size_t>(std::max<qsizetype>(0, s.capacity())) * sizeof(QChar);
}

template <typename Stack> [[nodiscard]] size_t recalculateStackBytes(const Stack &stack)
{
    size_t total = 0;
    for (const auto &entry : stack) {
        if (entry) {
            total += std::max<size_t>(sizeof(ICommand), entry->memoryFootprint());
        }
    }
    return total;
}

[[nodiscard]] QStringList parseTargetOutputsSpec(const QString &outputSpec)
{
    QStringList outputs;
    const QStringList parts = outputSpec.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &raw : parts) {
        const QString trimmed = raw.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        if (trimmed == QStringLiteral("*") ||
            trimmed.compare(QStringLiteral("All Displays"), Qt::CaseInsensitive) == 0) {
            // Wildcard output spec targets all connected displays in the IPC protocol.
            return {};
        }
        outputs.push_back(trimmed);
    }
    return outputs;
}

[[nodiscard]] WallpaperApplyOptions wallpaperOptionsFromMetadata(const QString &outputSpec,
                                                                 const waywal_img_metadata_t &meta,
                                                                 const QString &path)
{
    WallpaperApplyOptions opts;
    opts.targetOutputs = parseTargetOutputsSpec(outputSpec);
    const QString trimmedPath = path.trimmed();
    opts.filepath = !trimmedPath.isEmpty() ? trimmedPath : QString::fromUtf8(meta.filepath);
    opts.depthMapPath = QString::fromUtf8(meta.depth_map_path);
    opts.backdrop = QString::fromUtf8(meta.backdrop);
    opts.layerName = QString::fromUtf8(meta.layer_name);
    opts.batteryPolicy = QString::fromUtf8(meta.battery_policy);
    opts.scaling_mode = meta.scaling_mode;
    opts.filter_mode = meta.filter_mode;
    opts.transition_type = meta.transition_type;
    opts.transition_duration_ms = meta.transition_duration_ms;
    opts.transition_fps = meta.transition_fps;
    opts.transition_angle_rad = meta.transition_angle_rad;
    opts.transition_wave_freq = meta.transition_wave_freq;
    opts.transition_wave_amp = meta.transition_wave_amp;
    opts.transition_center_x = meta.transition_center_x;
    opts.transition_center_y = meta.transition_center_y;
    // waywal_img_metadata_t is #pragma pack(1); copy bezier bytes to avoid misaligned ref binding.
    std::memcpy(&opts.bezier,
                reinterpret_cast<const char *>(&meta) + offsetof(waywal_img_metadata_t, bezier),
                sizeof(opts.bezier));
    opts.has_custom_bezier = meta.has_custom_bezier;
    opts.crop_gravity_x = meta.crop_gravity_x;
    opts.crop_gravity_y = meta.crop_gravity_y;
    opts.has_crop_gravity = (meta.has_crop_gravity != 0u);
    opts.depth_strength = meta.depth_strength;
    opts.sync_mode = meta.sync_mode;
    opts.stagger_delay_ms = meta.stagger_delay_ms;
    opts.ten_bit = meta.ten_bit || (meta.pixel_format == WAYWAL_FORMAT_XRGB2101010);
    opts.invert_y = (meta.invert_y != 0u);
    opts.pos_y_inverted = (meta.pos_y_inverted != 0u);
    opts.no_cache = (meta.no_cache != 0u);
    opts.extract_theme = meta.extract_theme;
    opts.no_theme = meta.no_theme;
    opts.has_theme_mode = (meta.has_theme_mode != 0u);
    opts.theme_light_mode = meta.theme_light_mode;
    opts.is_solid_color = (meta.is_solid_color != 0u);
    std::memcpy(opts.color, meta.color, sizeof(opts.color));
    opts.has_seed_color = (meta.has_seed_color != 0u);
    opts.seed_color = meta.seed_color;
    opts.has_source_color_index = (meta.has_source_color_index != 0u);
    opts.source_color_index = meta.source_color_index;
    opts.has_auto_day_night = (meta.has_auto_day_night != 0u);
    opts.auto_day_night = (meta.auto_day_night != 0u);
    opts.has_solar_coords = (meta.has_solar_coords != 0u);
    opts.solar_latitude = meta.solar_latitude;
    opts.solar_longitude = meta.solar_longitude;
    return opts;
}

class BusArenaState;

std::atomic<uint64_t> g_busArenaEpoch{1ULL};

struct ThreadLocalBusCache {
    const CommandBus *bus{nullptr};
    BusArenaState *state{nullptr};
    uint64_t epoch{0ULL};
    uint32_t lastCoarseMonoMs{0U};
};

thread_local ThreadLocalBusCache t_busArenaCache{};

[[nodiscard]] std::chrono::steady_clock::time_point fastMonotonicNow() noexcept
{
#if defined(CLOCK_MONOTONIC_COARSE)
    struct timespec ts{};
    if (::clock_gettime(CLOCK_MONOTONIC_COARSE, &ts) == 0) {
        t_busArenaCache.lastCoarseMonoMs =
            static_cast<uint32_t>(static_cast<uint64_t>(ts.tv_sec) * 1000ULL +
                                  static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL);
        const auto durNs = std::chrono::nanoseconds(static_cast<int64_t>(ts.tv_sec) * 1000000000LL +
                                                    static_cast<int64_t>(ts.tv_nsec));
        return std::chrono::steady_clock::time_point(
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(durNs));
    }
#endif
    const auto now = std::chrono::steady_clock::now();
    t_busArenaCache.lastCoarseMonoMs = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
    return now;
}

[[nodiscard]] uint32_t fastMonotonicMs() noexcept
{
#if defined(CLOCK_MONOTONIC_COARSE)
    struct timespec ts{};
    if (::clock_gettime(CLOCK_MONOTONIC_COARSE, &ts) == 0) {
        const uint32_t ms = static_cast<uint32_t>(static_cast<uint64_t>(ts.tv_sec) * 1000ULL +
                                                  static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL);
        t_busArenaCache.lastCoarseMonoMs = ms;
        return ms;
    }
#endif
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    t_busArenaCache.lastCoarseMonoMs = static_cast<uint32_t>(ms);
    return t_busArenaCache.lastCoarseMonoMs;
}

[[nodiscard]] uint32_t hashUniformKey(const QString &output, const QString &name) noexcept
{
    uint32_t h = 2166136261U;
    auto mixBytes = [&h](const void *data, std::size_t len) noexcept {
        const auto *p = static_cast<const uint8_t *>(data);
        for (std::size_t i = 0; i < len; ++i) {
            h ^= p[i];
            h *= 16777619U;
        }
    };
    const uint32_t outLen = static_cast<uint32_t>(std::max<qsizetype>(0, output.size()));
    const uint32_t nameLen = static_cast<uint32_t>(std::max<qsizetype>(0, name.size()));
    mixBytes(&outLen, sizeof(outLen));
    if (outLen > 0U) {
        mixBytes(output.utf16(), static_cast<std::size_t>(outLen) * sizeof(char16_t));
    }
    mixBytes(&nameLen, sizeof(nameLen));
    if (nameLen > 0U) {
        mixBytes(name.utf16(), static_cast<std::size_t>(nameLen) * sizeof(char16_t));
    }
    return h;
}

constexpr uint32_t kRingTagSetUniform = 1U;
constexpr uint32_t kRingTagSetBezier = 2U;
constexpr uint32_t kRingTagSetNodeConst = 3U;

class BusArenaState final : public QObject {
  public:
    static constexpr std::size_t kRingCapacity = 4096U;

    explicit BusArenaState(CommandBus *owner) : QObject(owner) {}
    ~BusArenaState() override;

    void drainCommitted() noexcept
    {
        CommandRingArena<kRingCapacity>::EntryHeader hdr{};
        std::array<uint8_t, CommandRingArena<kRingCapacity>::kMaxCoalescedPayload> scratch{};
        while (arena.used_bytes() > (kRingCapacity / 2U)) {
            if (!arena.try_pop(hdr, scratch.data(), scratch.size())) {
                break;
            }
        }
    }

    void drainAll() noexcept
    {
        CommandRingArena<kRingCapacity>::EntryHeader hdr{};
        std::array<uint8_t, CommandRingArena<kRingCapacity>::kMaxCoalescedPayload> scratch{};
        while (arena.try_pop(hdr, scratch.data(), scratch.size())) {
        }
    }

    void flushAndDrain() noexcept
    {
        if (!arena.flush_coalesced()) {
            drainAll();
            (void)arena.flush_coalesced();
        }
        if (arena.used_bytes() > (kRingCapacity / 2U)) {
            drainCommitted();
        }
    }

    void pushToArena(uint32_t typeTag, uint32_t hash, uint32_t nowMs, const void *payload,
                     uint32_t payloadBytes) noexcept
    {
        if (!arena.try_push_or_coalesce(typeTag, hash, nowMs, payload, payloadBytes)) {
            drainAll();
            (void)arena.try_push_or_coalesce(typeTag, hash, nowMs, payload, payloadBytes);
        }
        if (arena.used_bytes() > (kRingCapacity / 2U)) {
            drainCommitted();
        }
    }

    [[gnu::noinline]] void pushFallback(uint32_t typeTag, uint32_t hash, uint32_t nowMs,
                                        const void *payload, uint32_t payloadBytes) noexcept
    {
        pushToArena(typeTag, hash, nowMs, payload, payloadBytes);
    }

    [[gnu::noinline]] void bindAndPushSetUniform(ICommand *rawTail, const SetUniformCommand *cmd,
                                                 uint32_t nowMs) noexcept
    {
        const auto &vals = cmd->newValues();
        const uint32_t payloadBytes = static_cast<uint32_t>(vals.size() * sizeof(float));
        const uint32_t uniformHash = hashUniformKey(cmd->output(), cmd->uniformName());
        topCmdPtr = rawTail;
        topTypeTag = kRingTagSetUniform;
        topUniformHash = uniformHash;
        topOutput = cmd->output();
        topUniformName = cmd->uniformName();
        topValueCount = static_cast<uint32_t>(vals.size());
        pushToArena(kRingTagSetUniform, uniformHash, nowMs, vals.data(), payloadBytes);
    }

    void clearTop() noexcept
    {
        topCmdPtr = nullptr;
        topTypeTag = 0U;
        topUniformHash = 0U;
        topValueCount = 0U;
    }

    bool *activeDestroyFlag{nullptr};
    CommandRingArena<kRingCapacity> arena;
    ICommand *topCmdPtr{nullptr};
    uint32_t topTypeTag{0U};
    uint32_t topUniformHash{0U};
    uint32_t topValueCount{0U};
    QString topOutput;
    QString topUniformName;
};

BusArenaState::~BusArenaState()
{
    if (activeDestroyFlag) {
        *activeDestroyFlag = true;
    }
    g_busArenaEpoch.fetch_add(1ULL, std::memory_order_acq_rel);
    if (t_busArenaCache.state == this) {
        t_busArenaCache.bus = nullptr;
        t_busArenaCache.state = nullptr;
        t_busArenaCache.epoch = 0ULL;
    }
}

struct FastArenaDispatchGuard {
    FastArenaDispatchGuard(bool &dispatching, BusArenaState *state, bool &destroyed) noexcept
        : m_dispatching(dispatching), m_state(state), m_destroyed(destroyed)
    {
        m_prevFlag = m_state->activeDestroyFlag;
        m_state->activeDestroyFlag = &m_destroyed;
        m_dispatching = true;
    }
    ~FastArenaDispatchGuard() noexcept
    {
        if (!m_destroyed) {
            m_dispatching = false;
            m_state->activeDestroyFlag = m_prevFlag;
        }
    }
    FastArenaDispatchGuard(const FastArenaDispatchGuard &) = delete;
    FastArenaDispatchGuard &operator=(const FastArenaDispatchGuard &) = delete;

  private:
    bool &m_dispatching;
    BusArenaState *m_state;
    bool &m_destroyed;
    bool *m_prevFlag{nullptr};
};

[[gnu::noinline]] BusArenaState *findArenaStateSlow(const CommandBus *bus, bool createIfMissing,
                                                    ThreadLocalBusCache &cache)
{
    BusArenaState *found = nullptr;
    for (QObject *child : bus->children()) {
        if (auto *candidate = dynamic_cast<BusArenaState *>(child)) {
            found = candidate;
            break;
        }
    }
    if (!found && createIfMissing) {
        found = new BusArenaState(const_cast<CommandBus *>(bus));
    }
    if (found) {
        cache.bus = bus;
        cache.state = found;
        cache.epoch = g_busArenaEpoch.load(std::memory_order_acquire);
    }
    return found;
}

[[nodiscard]] BusArenaState *findArenaState(const CommandBus *bus, bool createIfMissing,
                                            uint32_t *nowMsOut = nullptr)
{
    ThreadLocalBusCache &cache = t_busArenaCache;
    if (nowMsOut) {
        *nowMsOut = (cache.lastCoarseMonoMs != 0U) ? cache.lastCoarseMonoMs : fastMonotonicMs();
    }
    const uint64_t epoch = g_busArenaEpoch.load(std::memory_order_acquire);
    if (cache.bus == bus && cache.epoch == epoch) [[likely]] {
        return cache.state;
    }
    return findArenaStateSlow(bus, createIfMissing, cache);
}

[[nodiscard]] bool hasStableDescribeAcrossMerge(const ICommand &cmd) noexcept
{
    const std::type_info &ti = typeid(cmd);
    return ti == typeid(SetUniformCommand) || ti == typeid(SetBezierCurveCommand) ||
           ti == typeid(MoveNodesCommand) || ti == typeid(SetNodeConstantCommand) ||
           ti == typeid(SetCamera3DCommand) || ti == typeid(SetPhysicsSpringCommand);
}

template <typename T>
[[gnu::noinline]] const T *dynamicCastCommandFallback(const ICommand *other) noexcept
{
    return dynamic_cast<const T *>(other);
}

template <typename T> [[nodiscard]] const T *fastCastCommand(const ICommand *other) noexcept
{
    if (!other) [[unlikely]] {
        return nullptr;
    }
    if (typeid(*other) == typeid(T)) [[likely]] {
        return static_cast<const T *>(other);
    }
    return dynamicCastCommandFallback<T>(other);
}

constexpr int64_t kCoalesceWindowTicks =
    std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(500))
        .count();

[[nodiscard]] bool withinCoalesceWindowMs(std::chrono::steady_clock::time_point prev,
                                          std::chrono::steady_clock::time_point next) noexcept
{
    const auto delta = (next - prev).count();
    return static_cast<uint64_t>(delta) <= static_cast<uint64_t>(kCoalesceWindowTicks);
}

} // namespace

struct CommandBus::DispatchGuard {
    explicit DispatchGuard(QPointer<CommandBus> &owner) : m_owner(owner)
    {
        if (m_owner) {
            m_owner->m_dispatching = true;
        }
    }
    ~DispatchGuard()
    {
        if (m_owner) {
            m_owner->m_dispatching = false;
        }
    }
    DispatchGuard(const DispatchGuard &) = delete;
    DispatchGuard &operator=(const DispatchGuard &) = delete;

  private:
    QPointer<CommandBus> &m_owner;
};

/* ========================================================================= */
/* CommandBus Implementation                                                 */
/* ========================================================================= */

CommandBus::CommandBus(size_t maxHistory, QObject *parent)
    : QObject(parent), m_maxHistory(maxHistory > 0 ? maxHistory : 1000)
{
}

size_t CommandBus::commandFootprint(const ICommand &cmd) const
{
    return std::max<size_t>(sizeof(ICommand), cmd.memoryFootprint());
}

bool CommandBus::evictExcessHistory()
{
    bool evicted = false;
    while (m_undoStack.size() > m_maxHistory ||
           (!m_undoStack.empty() && m_undoBytes > m_maxHistoryBytes)) {
        const size_t frontBytes = commandFootprint(*m_undoStack.front());
        m_undoStack.pop_front();
        m_undoBytes = (m_undoBytes >= frontBytes) ? (m_undoBytes - frontBytes)
                                                  : recalculateStackBytes(m_undoStack);
        evicted = true;
    }
    return evicted;
}

void CommandBus::setMaxHistoryBytes(size_t maxBytes)
{
    m_maxHistoryBytes = maxBytes > 0 ? maxBytes : DEFAULT_MAX_HISTORY_BYTES;
    bool changed = evictExcessHistory();
    while (!m_redoStack.empty() && m_redoBytes > m_maxHistoryBytes) {
        const size_t frontBytes = commandFootprint(*m_redoStack.front());
        m_redoStack.erase(m_redoStack.begin());
        m_redoBytes = (m_redoBytes >= frontBytes) ? (m_redoBytes - frontBytes)
                                                  : recalculateStackBytes(m_redoStack);
        changed = true;
    }
    if (changed) {
        if (BusArenaState *state = findArenaState(this, false)) {
            if (m_undoStack.empty() || m_undoStack.back().get() != state->topCmdPtr) {
                state->flushAndDrain();
                state->clearTop();
            }
        }
        notifyHistoryChanged();
        emit stateChanged();
    }
}

void CommandBus::execute(std::unique_ptr<ICommand> cmd) { (void)tryExecute(std::move(cmd)); }

void CommandBus::undo() { (void)tryUndo(); }

void CommandBus::redo() { (void)tryRedo(); }

bool CommandBus::tryExecute(std::unique_ptr<ICommand> cmd)
{
    if (!cmd)
        return false;

    if (m_dispatching) {
        emit commandFailed(QStringLiteral("CommandBus::execute() re-entrant dispatch rejected"));
        return false;
    }

    if (typeid(*cmd) == typeid(SetUniformCommand)) [[likely]] {
        auto *inCmd = static_cast<SetUniformCommand *>(cmd.get());
        uint32_t nowMs = 0U;
        BusArenaState *state = findArenaState(this, true, &nowMs);
        const auto &newVals = inCmd->newValues();
        const uint32_t payloadBytes = static_cast<uint32_t>(newVals.size() * sizeof(float));

        if (!m_undoStack.empty()) [[likely]] {
            ICommand *rawTail = m_undoStack.back().get();
            if (typeid(*rawTail) == typeid(SetUniformCommand)) [[likely]] {
                auto *tail = static_cast<SetUniformCommand *>(rawTail);
                if (tail->SetUniformCommand::mergeWith(inCmd)) [[likely]] {
                    if (state->topCmdPtr == rawTail && state->topTypeTag == kRingTagSetUniform)
                        [[likely]] {
                        if (!state->arena.try_coalesce_staged(
                                kRingTagSetUniform, state->topUniformHash, nowMs, newVals.data(),
                                payloadBytes)) [[unlikely]] {
                            state->pushFallback(kRingTagSetUniform, state->topUniformHash, nowMs,
                                                newVals.data(), payloadBytes);
                        }
                    } else {
                        state->bindAndPushSetUniform(rawTail, inCmd, nowMs);
                    }

                    bool busDestroyed = false;
                    QString errorReason;
                    {
                        FastArenaDispatchGuard guard(m_dispatching, state, busDestroyed);
                        try {
                            tail->SetUniformCommand::execute();
                        } catch (const std::exception &ex) {
                            errorReason = QString::fromUtf8(ex.what());
                        } catch (...) {
                            errorReason =
                                QStringLiteral("Unknown exception during coalesced execute()");
                        }
                    }
                    if (busDestroyed) [[unlikely]] {
                        return false;
                    }

                    bool historyMetaChanged = false;
                    if (!m_redoStack.empty()) [[unlikely]] {
                        m_redoStack.clear();
                        m_redoBytes = 0;
                        historyMetaChanged = true;
                    }

                    if (!errorReason.isEmpty()) [[unlikely]] {
                        emit commandFailed(errorReason);
                        notifyHistoryChanged();
                        emit stateChanged();
                        return false;
                    }
                    if (historyMetaChanged) [[unlikely]] {
                        notifyHistoryChanged();
                        emit stateChanged();
                    }
                    return true;
                }
            }
        }

        if (!inCmd->SetUniformCommand::isValid()) {
            emit commandFailed(
                QStringLiteral("CommandBus rejected invalid command: %1").arg(inCmd->describe()));
            return false;
        }

        bool busDestroyed = false;
        QString errorReason;
        {
            FastArenaDispatchGuard guard(m_dispatching, state, busDestroyed);
            try {
                inCmd->SetUniformCommand::execute();
            } catch (const std::exception &ex) {
                errorReason = QString::fromUtf8(ex.what());
            } catch (...) {
                errorReason = QStringLiteral("Unknown exception during command execute()");
            }
        }

        if (busDestroyed) {
            return false;
        }
        if (!errorReason.isEmpty()) {
            emit commandFailed(errorReason);
            return false;
        }

        state->flushAndDrain();
        state->bindAndPushSetUniform(inCmd, inCmd, nowMs);

        const size_t cmdBytes = commandFootprint(*inCmd);

        m_undoStack.push_back(std::move(cmd));
        m_undoBytes += cmdBytes;
        if (!m_redoStack.empty()) {
            m_redoStack.clear();
            m_redoBytes = 0;
        }
        evictExcessHistory();
        if (m_undoStack.empty() || m_undoStack.back().get() != state->topCmdPtr) {
            state->flushAndDrain();
            state->clearTop();
        }
        notifyHistoryChanged();
        emit stateChanged();
        return true;
    }

    if (!cmd->isValid()) {
        emit commandFailed(
            QStringLiteral("CommandBus rejected invalid command: %1").arg(cmd->describe()));
        return false;
    }

    QPointer<CommandBus> self(this);
    QString errorReason;

    if (!m_undoStack.empty()) {
        ICommand *tail = m_undoStack.back().get();
        const size_t oldTailBytes = commandFootprint(*tail);
        const bool stableLabel = hasStableDescribeAcrossMerge(*tail);
        const QString prevDesc = stableLabel ? QString() : tail->describe();
        if (tail->mergeWith(cmd.get())) {
            if (typeid(*tail) == typeid(SetBezierCurveCommand)) {
                auto *bTail = static_cast<SetBezierCurveCommand *>(tail);
                BusArenaState *state = findArenaState(this, true);
                state->pushToArena(kRingTagSetBezier, hashUniformKey(bTail->output(), QString()),
                                   fastMonotonicMs(), &bTail->newCurve(),
                                   static_cast<uint32_t>(sizeof(bezier_curve_t)));
                state->topCmdPtr = tail;
                state->topTypeTag = kRingTagSetBezier;
            } else if (typeid(*tail) == typeid(SetNodeConstantCommand)) {
                auto *cTail = static_cast<SetNodeConstantCommand *>(tail);
                BusArenaState *state = findArenaState(this, true);
                state->pushToArena(kRingTagSetNodeConst, cTail->nodeId(), fastMonotonicMs(),
                                   cTail->newVal().data(),
                                   static_cast<uint32_t>(sizeof(std::array<float, 4>)));
                state->topCmdPtr = tail;
                state->topTypeTag = kRingTagSetNodeConst;
            }

            {
                DispatchGuard guard(self);
                try {
                    tail->execute();
                } catch (const std::exception &ex) {
                    errorReason = QString::fromUtf8(ex.what());
                } catch (...) {
                    errorReason = QStringLiteral("Unknown exception during coalesced execute()");
                }
            }
            if (!self) {
                return false;
            }
            // A coalesced edit starts a fresh future branch just like an unmerged execute().
            const bool hadRedo = !m_redoStack.empty();
            if (hadRedo) {
                m_redoStack.clear();
                m_redoBytes = 0;
            }

            const size_t newTailBytes = commandFootprint(*tail);
            m_undoBytes =
                (m_undoBytes >= oldTailBytes ? m_undoBytes - oldTailBytes : 0) + newTailBytes;
            const bool evicted = evictExcessHistory();
            if (evicted) {
                if (BusArenaState *state = findArenaState(this, false)) {
                    if (m_undoStack.empty() || m_undoStack.back().get() != state->topCmdPtr) {
                        state->flushAndDrain();
                        state->clearTop();
                    }
                }
            }

            if (!errorReason.isEmpty()) {
                emit commandFailed(errorReason);
                notifyHistoryChanged();
                emit stateChanged();
                return false;
            }
            const bool labelChanged = !stableLabel && (tail->describe() != prevDesc);
            if (hadRedo || evicted || labelChanged) {
                notifyHistoryChanged();
                emit stateChanged();
            }
            return true;
        }
    }

    {
        DispatchGuard guard(self);
        try {
            cmd->execute();
        } catch (const std::exception &ex) {
            errorReason = QString::fromUtf8(ex.what());
        } catch (...) {
            errorReason = QStringLiteral("Unknown exception during command execute()");
        }
    }

    if (!self) {
        return false;
    }

    if (!errorReason.isEmpty()) {
        emit commandFailed(errorReason);
        return false;
    }

    if (typeid(*cmd) == typeid(SetBezierCurveCommand)) {
        auto *bCmd = static_cast<SetBezierCurveCommand *>(cmd.get());
        BusArenaState *state = findArenaState(this, true);
        state->flushAndDrain();
        state->pushToArena(kRingTagSetBezier, hashUniformKey(bCmd->output(), QString()),
                           fastMonotonicMs(), &bCmd->newCurve(),
                           static_cast<uint32_t>(sizeof(bezier_curve_t)));
        state->topCmdPtr = bCmd;
        state->topTypeTag = kRingTagSetBezier;
    } else if (typeid(*cmd) == typeid(SetNodeConstantCommand)) {
        auto *cCmd = static_cast<SetNodeConstantCommand *>(cmd.get());
        BusArenaState *state = findArenaState(this, true);
        state->flushAndDrain();
        state->pushToArena(kRingTagSetNodeConst, cCmd->nodeId(), fastMonotonicMs(),
                           cCmd->newVal().data(),
                           static_cast<uint32_t>(sizeof(std::array<float, 4>)));
        state->topCmdPtr = cCmd;
        state->topTypeTag = kRingTagSetNodeConst;
    } else if (BusArenaState *state = findArenaState(this, false)) {
        state->flushAndDrain();
        state->clearTop();
    }

    const size_t cmdBytes = commandFootprint(*cmd);
    m_undoStack.push_back(std::move(cmd));
    m_undoBytes += cmdBytes;
    if (!m_redoStack.empty()) {
        m_redoStack.clear();
        m_redoBytes = 0;
    }

    evictExcessHistory();
    if (BusArenaState *state = findArenaState(this, false)) {
        if (m_undoStack.empty() || m_undoStack.back().get() != state->topCmdPtr) {
            state->flushAndDrain();
            state->clearTop();
        }
    }

    notifyHistoryChanged();
    emit stateChanged();
    return true;
}

bool CommandBus::tryUndo()
{
    if (m_dispatching) {
        emit commandFailed(QStringLiteral("CommandBus::undo() re-entrant dispatch rejected"));
        return false;
    }
    if (m_undoStack.empty())
        return false;

    if (BusArenaState *state = findArenaState(this, false)) {
        state->flushAndDrain();
        state->clearTop();
    }

    auto cmd = std::move(m_undoStack.back());
    m_undoStack.pop_back();
    const size_t cmdBytes = commandFootprint(*cmd);
    m_undoBytes =
        (m_undoBytes >= cmdBytes) ? (m_undoBytes - cmdBytes) : recalculateStackBytes(m_undoStack);

    // Move onto destination stack before invoking callbacks so listeners see consistent stacks.
    ICommand *rawCmd = cmd.get();
    m_redoStack.push_back(std::move(cmd));
    m_redoBytes += cmdBytes;

    QPointer<CommandBus> self(this);
    QString errorReason;
    {
        DispatchGuard guard(self);
        try {
            rawCmd->undo();
        } catch (const std::exception &ex) {
            errorReason = QString::fromUtf8(ex.what());
        } catch (...) {
            errorReason = QStringLiteral("Unknown exception during command undo()");
        }
    }

    if (!self) {
        return false;
    }

    if (!errorReason.isEmpty()) {
        if (!m_redoStack.empty() && m_redoStack.back().get() == rawCmd) {
            auto failed = std::move(m_redoStack.back());
            m_redoStack.pop_back();
            m_redoBytes = (m_redoBytes >= cmdBytes) ? (m_redoBytes - cmdBytes)
                                                    : recalculateStackBytes(m_redoStack);
            m_undoStack.push_back(std::move(failed));
            m_undoBytes += cmdBytes;
        }
        emit commandFailed(errorReason);
        return false;
    }

    notifyHistoryChanged();
    emit stateChanged();
    return true;
}

bool CommandBus::tryRedo()
{
    if (m_dispatching) {
        emit commandFailed(QStringLiteral("CommandBus::redo() re-entrant dispatch rejected"));
        return false;
    }
    if (m_redoStack.empty())
        return false;

    if (BusArenaState *state = findArenaState(this, false)) {
        state->flushAndDrain();
        state->clearTop();
    }

    auto cmd = std::move(m_redoStack.back());
    m_redoStack.pop_back();
    const size_t cmdBytes = commandFootprint(*cmd);
    m_redoBytes =
        (m_redoBytes >= cmdBytes) ? (m_redoBytes - cmdBytes) : recalculateStackBytes(m_redoStack);

    // Move onto destination stack before invoking callbacks so listeners see consistent stacks.
    ICommand *rawCmd = cmd.get();
    m_undoStack.push_back(std::move(cmd));
    m_undoBytes += cmdBytes;

    QPointer<CommandBus> self(this);
    QString errorReason;
    {
        DispatchGuard guard(self);
        try {
            rawCmd->redo();
        } catch (const std::exception &ex) {
            errorReason = QString::fromUtf8(ex.what());
        } catch (...) {
            errorReason = QStringLiteral("Unknown exception during command redo()");
        }
    }

    if (!self) {
        return false;
    }

    if (!errorReason.isEmpty()) {
        if (!m_undoStack.empty() && m_undoStack.back().get() == rawCmd) {
            auto failed = std::move(m_undoStack.back());
            m_undoStack.pop_back();
            m_undoBytes = (m_undoBytes >= cmdBytes) ? (m_undoBytes - cmdBytes)
                                                    : recalculateStackBytes(m_undoStack);
            m_redoStack.push_back(std::move(failed));
            m_redoBytes += cmdBytes;
        }
        emit commandFailed(errorReason);
        return false;
    }

    evictExcessHistory();
    notifyHistoryChanged();
    emit stateChanged();
    return true;
}

void CommandBus::clear()
{
    if (m_dispatching) {
        emit commandFailed(QStringLiteral("CommandBus::clear() re-entrant call rejected"));
        return;
    }
    if (BusArenaState *state = findArenaState(this, false)) {
        state->arena.reset();
        state->clearTop();
    }
    m_undoStack.clear();
    m_redoStack.clear();
    m_undoBytes = 0;
    m_redoBytes = 0;
    notifyHistoryChanged();
    emit stateChanged();
}

bool CommandBus::canUndo() const { return !m_undoStack.empty(); }

bool CommandBus::canRedo() const { return !m_redoStack.empty(); }

QString CommandBus::undoText() const
{
    if (m_undoStack.empty())
        return {};
    return m_undoStack.back()->describe();
}

QString CommandBus::redoText() const
{
    if (m_redoStack.empty())
        return {};
    return m_redoStack.back()->describe();
}

size_t CommandBus::undoCount() const { return m_undoStack.size(); }

size_t CommandBus::redoCount() const { return m_redoStack.size(); }

void CommandBus::notifyHistoryChanged()
{
    emit historyChanged(canUndo(), canRedo(), undoText(), redoText());
}

/* ========================================================================= */
/* SetUniformCommand Implementation                                          */
/* ========================================================================= */

SetUniformCommand::SetUniformCommand(QString output, QString uniformName,
                                     std::vector<float> oldValues, std::vector<float> newValues,
                                     uint32_t flags, ApplyFunction applyFunc)
    : m_output(std::move(output)), m_uniformName(std::move(uniformName)),
      m_oldValues(std::move(oldValues)), m_newValues(std::move(newValues)), m_flags(flags),
      m_applyFunc(std::move(applyFunc)), m_timestamp(fastMonotonicNow())
{
}

void SetUniformCommand::execute()
{
    if (const ApplyFunction fn = m_applyFunc) {
        fn(m_output, m_uniformName, m_newValues.data(), static_cast<uint32_t>(m_newValues.size()),
           m_flags);
    } else if (!m_newValues.empty()) {
        if (auto *ipc = Services::instance().ipcClient()) {
            ipc->sendSetUniform(m_output, m_uniformName, m_newValues.data(),
                                static_cast<uint32_t>(m_newValues.size()),
                                (m_flags & WAYWAL_UNIFORM_FLAG_PERSIST) != 0);
        }
    }
}

void SetUniformCommand::undo()
{
    if (const ApplyFunction fn = m_applyFunc) {
        fn(m_output, m_uniformName, m_oldValues.data(), static_cast<uint32_t>(m_oldValues.size()),
           m_flags);
    } else if (!m_oldValues.empty()) {
        if (auto *ipc = Services::instance().ipcClient()) {
            ipc->sendSetUniform(m_output, m_uniformName, m_oldValues.data(),
                                static_cast<uint32_t>(m_oldValues.size()),
                                (m_flags & WAYWAL_UNIFORM_FLAG_PERSIST) != 0);
        }
    }
}

QString SetUniformCommand::describe() const
{
    if (m_output.isEmpty()) {
        return QString("Set uniform '%1'").arg(m_uniformName);
    }
    return QString("Set uniform '%1' on %2").arg(m_uniformName, m_output);
}

bool SetUniformCommand::mergeWith(const ICommand *other)
{
    const auto *otherCmd = fastCastCommand<SetUniformCommand>(other);
    if (!otherCmd || m_flags != otherCmd->m_flags ||
        m_newValues.size() != otherCmd->m_newValues.size() ||
        !qStringsEqualFast(m_uniformName, otherCmd->m_uniformName) ||
        !qStringsEqualFast(m_output, otherCmd->m_output)) {
        return false;
    }

    // Coalesce continuous slider/color micro-drags within 500 ms into a single undo step
    if (!withinCoalesceWindowMs(m_timestamp, otherCmd->m_timestamp) || !otherCmd->isValid()) {
        return false;
    }

    const size_t valCount = m_newValues.size();
    if (valCount == 1u) {
        m_newValues.data()[0] = otherCmd->m_newValues.data()[0];
    } else if (valCount > 1u) {
        std::memcpy(m_newValues.data(), otherCmd->m_newValues.data(), valCount * sizeof(float));
    }
    if (otherCmd->m_applyFunc) {
        m_applyFunc = otherCmd->m_applyFunc;
    }
    m_timestamp = otherCmd->m_timestamp;
    return true;
}

size_t SetUniformCommand::memoryFootprint() const
{
    return sizeof(*this) + qStringHeapBytes(m_output) + qStringHeapBytes(m_uniformName) +
           ((m_oldValues.capacity() + m_newValues.capacity()) * sizeof(float));
}

bool SetUniformCommand::isValid() const
{
    return !m_uniformName.isEmpty() && !m_newValues.empty() && m_newValues.size() <= 16u &&
           m_oldValues.size() <= 16u && allFiniteVec(m_oldValues) && allFiniteVec(m_newValues);
}

/* ========================================================================= */
/* SetBezierCurveCommand Implementation                                      */
/* ========================================================================= */

SetBezierCurveCommand::SetBezierCurveCommand(QString output, bezier_curve_t oldCurve,
                                             bezier_curve_t newCurve, ApplyFunction applyFunc)
    : m_output(std::move(output)), m_oldCurve(oldCurve), m_newCurve(newCurve),
      m_applyFunc(std::move(applyFunc)), m_timestamp(fastMonotonicNow())
{
}

void SetBezierCurveCommand::execute()
{
    if (const ApplyFunction fn = m_applyFunc) {
        fn(m_output, m_newCurve);
    }
}

void SetBezierCurveCommand::undo()
{
    if (const ApplyFunction fn = m_applyFunc) {
        fn(m_output, m_oldCurve);
    }
}

QString SetBezierCurveCommand::describe() const
{
    if (m_output.isEmpty()) {
        return QString("Adjust Cubic-Bezier easing curve");
    }
    return QString("Adjust Cubic-Bezier easing curve on %1").arg(m_output);
}

bool SetBezierCurveCommand::mergeWith(const ICommand *other)
{
    const auto *otherCmd = fastCastCommand<SetBezierCurveCommand>(other);
    if (!otherCmd || !qStringsEqualFast(m_output, otherCmd->m_output) ||
        !withinCoalesceWindowMs(m_timestamp, otherCmd->m_timestamp) || !otherCmd->isValid()) {
        return false;
    }

    m_newCurve = otherCmd->m_newCurve;
    if (otherCmd->m_applyFunc) {
        m_applyFunc = otherCmd->m_applyFunc;
    }
    m_timestamp = otherCmd->m_timestamp;
    return true;
}

size_t SetBezierCurveCommand::memoryFootprint() const
{
    return sizeof(*this) + qStringHeapBytes(m_output);
}

bool SetBezierCurveCommand::isValid() const
{
    return isValidBezierCurve(m_oldCurve) && isValidBezierCurve(m_newCurve);
}

/* ========================================================================= */
/* ApplyWallpaperCommand Implementation                                      */
/* ========================================================================= */

ApplyWallpaperCommand::ApplyWallpaperCommand(QString output, waywal_img_metadata_t oldMeta,
                                             waywal_img_metadata_t newMeta, QString oldPath,
                                             QString newPath, ApplyFunction applyFunc)
    : m_output(std::move(output)), m_oldMeta(oldMeta), m_newMeta(newMeta),
      m_oldPath(std::move(oldPath)), m_newPath(std::move(newPath)),
      m_applyFunc(std::move(applyFunc))
{
}

void ApplyWallpaperCommand::execute()
{
    if (const ApplyFunction fn = m_applyFunc) {
        fn(m_output, m_newMeta, m_newPath);
    } else if (auto *ipc = Services::instance().ipcClient()) {
        // Fallback dispatch must preserve target monitor routing and explicit path via
        // WallpaperApplyOptions.
        ipc->setWallpaper(wallpaperOptionsFromMetadata(m_output, m_newMeta, m_newPath));
    }
}

void ApplyWallpaperCommand::undo()
{
    if (const ApplyFunction fn = m_applyFunc) {
        fn(m_output, m_oldMeta, m_oldPath);
    } else if (auto *ipc = Services::instance().ipcClient()) {
        ipc->setWallpaper(wallpaperOptionsFromMetadata(m_output, m_oldMeta, m_oldPath));
    }
}

QString ApplyWallpaperCommand::describe() const
{
    QString fileName = QFileInfo(m_newPath).fileName();
    if (fileName.isEmpty()) {
        fileName = m_newPath;
    }
    if (m_output.isEmpty()) {
        return QString("Apply wallpaper '%1'").arg(fileName);
    }
    return QString("Apply wallpaper '%1' to %2").arg(fileName, m_output);
}

size_t ApplyWallpaperCommand::memoryFootprint() const
{
    return sizeof(*this) + qStringHeapBytes(m_output) + qStringHeapBytes(m_oldPath) +
           qStringHeapBytes(m_newPath);
}

bool ApplyWallpaperCommand::isValid() const
{
    return !m_newPath.isEmpty() || static_cast<bool>(m_applyFunc);
}

/* ========================================================================= */
/* SetPowerPolicyCommand Implementation                                      */
/* ========================================================================= */

SetPowerPolicyCommand::SetPowerPolicyCommand(uint32_t oldPolicy, uint32_t newPolicy,
                                             ApplyFunction applyFunc)
    : m_oldPolicy(oldPolicy), m_newPolicy(newPolicy), m_applyFunc(std::move(applyFunc))
{
}

void SetPowerPolicyCommand::execute()
{
    if (const ApplyFunction fn = m_applyFunc) {
        fn(m_newPolicy);
    } else if (auto *ipc = Services::instance().ipcClient()) {
        ipc->sendAudioPowerConfig(-1.0f, -1, -1, static_cast<uint8_t>(m_newPolicy));
    }
}

void SetPowerPolicyCommand::undo()
{
    if (const ApplyFunction fn = m_applyFunc) {
        fn(m_oldPolicy);
    } else if (auto *ipc = Services::instance().ipcClient()) {
        ipc->sendAudioPowerConfig(-1.0f, -1, -1, static_cast<uint8_t>(m_oldPolicy));
    }
}

QString SetPowerPolicyCommand::describe() const
{
    const char *policyStr = "unknown";
    switch (m_newPolicy) {
    case 0:
        policyStr = "ignore";
        break;
    case 1:
        policyStr = "reduce-fps";
        break;
    case 2:
        policyStr = "pause-video";
        break;
    }
    return QString("Set battery policy: %1").arg(policyStr);
}

size_t SetPowerPolicyCommand::memoryFootprint() const { return sizeof(*this); }

bool SetPowerPolicyCommand::isValid() const { return (m_oldPolicy <= 255u && m_newPolicy <= 255u); }

/* ========================================================================= */
/* Shader Graph, 3D Camera & Puppet Physics Commands                         */
/* ========================================================================= */

AddNodeCommand::AddNodeCommand(waywal_graph_node_t node, MutateFn mutateFn)
    : m_node(node), m_mutateFn(std::move(mutateFn))
{
}

void AddNodeCommand::execute()
{
    if (const MutateFn fn = m_mutateFn) {
        fn(m_node, true);
    }
}

void AddNodeCommand::undo()
{
    if (const MutateFn fn = m_mutateFn) {
        fn(m_node, false);
    }
}

QString AddNodeCommand::describe() const
{
    return QStringLiteral("Add node '%1' (#%2)")
        .arg(QString::fromUtf8(m_node.title), QString::number(m_node.node_id));
}

size_t AddNodeCommand::memoryFootprint() const { return sizeof(*this); }

RemoveNodeCommand::RemoveNodeCommand(waywal_graph_node_t removedNode,
                                     std::vector<waywal_graph_edge_t> detachedEdges,
                                     RestoreFn restoreFn)
    : m_removedNode(removedNode), m_detachedEdges(std::move(detachedEdges)),
      m_restoreFn(std::move(restoreFn))
{
}

void RemoveNodeCommand::execute()
{
    if (const RestoreFn fn = m_restoreFn) {
        fn(m_removedNode, m_detachedEdges, false);
    }
}

void RemoveNodeCommand::undo()
{
    if (const RestoreFn fn = m_restoreFn) {
        fn(m_removedNode, m_detachedEdges, true);
    }
}

QString RemoveNodeCommand::describe() const
{
    return QStringLiteral("Remove node '%1' (#%2)")
        .arg(QString::fromUtf8(m_removedNode.title), QString::number(m_removedNode.node_id));
}

size_t RemoveNodeCommand::memoryFootprint() const
{
    return sizeof(*this) + (m_detachedEdges.capacity() * sizeof(waywal_graph_edge_t));
}

MoveNodesCommand::MoveNodesCommand(std::vector<NodeMoveDelta> deltas, ApplyMoveFn applyFn)
    : m_deltas(std::move(deltas)), m_applyFn(std::move(applyFn)), m_timestamp(fastMonotonicNow())
{
}

void MoveNodesCommand::execute()
{
    if (const ApplyMoveFn fn = m_applyFn) {
        fn(m_deltas, true);
    }
}

void MoveNodesCommand::undo()
{
    if (const ApplyMoveFn fn = m_applyFn) {
        fn(m_deltas, false);
    }
}

QString MoveNodesCommand::describe() const
{
    return QStringLiteral("Move %1 node(s)").arg(m_deltas.size());
}

bool MoveNodesCommand::mergeWith(const ICommand *other)
{
    const auto *otherCmd = fastCastCommand<MoveNodesCommand>(other);
    if (!otherCmd || m_deltas.size() != otherCmd->m_deltas.size() ||
        !withinCoalesceWindowMs(m_timestamp, otherCmd->m_timestamp) || !otherCmd->isValid()) {
        return false;
    }

    for (size_t i = 0; i < m_deltas.size(); ++i) {
        if (m_deltas[i].nodeId != otherCmd->m_deltas[i].nodeId) {
            return false;
        }
    }

    for (size_t i = 0; i < m_deltas.size(); ++i) {
        m_deltas[i].newPos = otherCmd->m_deltas[i].newPos;
    }
    if (otherCmd->m_applyFn) {
        m_applyFn = otherCmd->m_applyFn;
    }
    m_timestamp = otherCmd->m_timestamp;
    return true;
}

size_t MoveNodesCommand::memoryFootprint() const
{
    return sizeof(*this) + (m_deltas.capacity() * sizeof(NodeMoveDelta));
}

bool MoveNodesCommand::isValid() const
{
    if (m_deltas.empty()) {
        return false;
    }
    for (const auto &d : m_deltas) {
        if (!std::isfinite(d.oldPos.x()) || !std::isfinite(d.oldPos.y()) ||
            !std::isfinite(d.newPos.x()) || !std::isfinite(d.newPos.y())) {
            return false;
        }
    }
    return true;
}

ConnectWireCommand::ConnectWireCommand(waywal_graph_edge_t edge, WireFn wireFn,
                                       std::optional<waywal_graph_edge_t> replacedEdge)
    : m_edge(edge), m_replacedEdge(replacedEdge), m_wireFn(std::move(wireFn))
{
}

void ConnectWireCommand::execute()
{
    if (const WireFn fn = m_wireFn) {
        if (m_replacedEdge.has_value()) {
            fn(*m_replacedEdge, false);
        }
        fn(m_edge, true);
    }
}

void ConnectWireCommand::undo()
{
    if (const WireFn fn = m_wireFn) {
        fn(m_edge, false);
        if (m_replacedEdge.has_value()) {
            fn(*m_replacedEdge, true);
        }
    }
}

QString ConnectWireCommand::describe() const
{
    return QStringLiteral("Connect wire (#%1: %2:%3 -> %4:%5)")
        .arg(QString::number(m_edge.edge_id), QString::number(m_edge.src_node_id),
             QString::number(m_edge.src_pin_index), QString::number(m_edge.dst_node_id),
             QString::number(m_edge.dst_pin_index));
}

DisconnectWireCommand::DisconnectWireCommand(waywal_graph_edge_t edge, WireFn wireFn)
    : m_edge(edge), m_wireFn(std::move(wireFn))
{
}

void DisconnectWireCommand::execute()
{
    if (const WireFn fn = m_wireFn) {
        fn(m_edge, false);
    }
}

void DisconnectWireCommand::undo()
{
    if (const WireFn fn = m_wireFn) {
        fn(m_edge, true);
    }
}

QString DisconnectWireCommand::describe() const
{
    return QStringLiteral("Disconnect wire (#%1: %2:%3 -> %4:%5)")
        .arg(QString::number(m_edge.edge_id), QString::number(m_edge.src_node_id),
             QString::number(m_edge.src_pin_index), QString::number(m_edge.dst_node_id),
             QString::number(m_edge.dst_pin_index));
}

SetNodeConstantCommand::SetNodeConstantCommand(uint32_t nodeId, QString nodeTitle,
                                               std::array<float, 4> oldVal,
                                               std::array<float, 4> newVal, ApplyConstFn applyFn)
    : m_nodeId(nodeId), m_nodeTitle(std::move(nodeTitle)), m_oldVal(oldVal), m_newVal(newVal),
      m_applyFn(std::move(applyFn)), m_timestamp(fastMonotonicNow())
{
}

void SetNodeConstantCommand::execute()
{
    if (const ApplyConstFn fn = m_applyFn) {
        fn(m_nodeId, m_newVal);
    }
}

void SetNodeConstantCommand::undo()
{
    if (const ApplyConstFn fn = m_applyFn) {
        fn(m_nodeId, m_oldVal);
    }
}

QString SetNodeConstantCommand::describe() const
{
    return QStringLiteral("Set constant on '%1' (#%2)").arg(m_nodeTitle, QString::number(m_nodeId));
}

bool SetNodeConstantCommand::mergeWith(const ICommand *other)
{
    const auto *otherCmd = fastCastCommand<SetNodeConstantCommand>(other);
    if (!otherCmd || m_nodeId != otherCmd->m_nodeId ||
        !withinCoalesceWindowMs(m_timestamp, otherCmd->m_timestamp) || !otherCmd->isValid()) {
        return false;
    }

    m_newVal = otherCmd->m_newVal;
    if (otherCmd->m_applyFn) {
        m_applyFn = otherCmd->m_applyFn;
    }
    m_timestamp = otherCmd->m_timestamp;
    return true;
}

size_t SetNodeConstantCommand::memoryFootprint() const
{
    return sizeof(*this) + qStringHeapBytes(m_nodeTitle);
}

bool SetNodeConstantCommand::isValid() const
{
    return allFiniteArr4(m_oldVal) && allFiniteArr4(m_newVal);
}

SetCamera3DCommand::SetCamera3DCommand(QString output, we_camera3d_t oldCam, we_camera3d_t newCam,
                                       we_lighting_uniforms_t oldLights,
                                       we_lighting_uniforms_t newLights, ApplyCamFn applyFn)
    : m_output(std::move(output)), m_oldCam(oldCam), m_newCam(newCam), m_oldLights(oldLights),
      m_newLights(newLights), m_applyFn(std::move(applyFn)), m_timestamp(fastMonotonicNow())
{
}

void SetCamera3DCommand::execute()
{
    if (const ApplyCamFn fn = m_applyFn) {
        fn(m_output, m_newCam, m_newLights);
    }
}

void SetCamera3DCommand::undo()
{
    if (const ApplyCamFn fn = m_applyFn) {
        fn(m_output, m_oldCam, m_oldLights);
    }
}

QString SetCamera3DCommand::describe() const
{
    if (m_output.isEmpty()) {
        return QStringLiteral("Adjust 3D Camera & Lighting");
    }
    return QStringLiteral("Adjust 3D Camera & Lighting on %1").arg(m_output);
}

bool SetCamera3DCommand::mergeWith(const ICommand *other)
{
    const auto *otherCmd = fastCastCommand<SetCamera3DCommand>(other);
    if (!otherCmd || !qStringsEqualFast(m_output, otherCmd->m_output) ||
        !withinCoalesceWindowMs(m_timestamp, otherCmd->m_timestamp)) {
        return false;
    }

    m_newCam = otherCmd->m_newCam;
    m_newLights = otherCmd->m_newLights;
    if (otherCmd->m_applyFn) {
        m_applyFn = otherCmd->m_applyFn;
    }
    m_timestamp = otherCmd->m_timestamp;
    return true;
}

size_t SetCamera3DCommand::memoryFootprint() const
{
    return sizeof(*this) + qStringHeapBytes(m_output);
}

SetPhysicsSpringCommand::SetPhysicsSpringCommand(QString output, int32_t objectId,
                                                 int32_t boneIndex, we_bone_sim_config_t oldCfg,
                                                 we_bone_sim_config_t newCfg,
                                                 we_wind_field_t oldWind, we_wind_field_t newWind,
                                                 we_cursor_force_t oldCursor,
                                                 we_cursor_force_t newCursor, ApplyPhysFn applyFn)
    : m_output(std::move(output)), m_objectId(objectId), m_boneIndex(boneIndex), m_oldCfg(oldCfg),
      m_newCfg(newCfg), m_oldWind(oldWind), m_newWind(newWind), m_oldCursor(oldCursor),
      m_newCursor(newCursor), m_applyFn(std::move(applyFn)), m_timestamp(fastMonotonicNow())
{
}

void SetPhysicsSpringCommand::execute()
{
    if (const ApplyPhysFn fn = m_applyFn) {
        fn(m_output, m_objectId, m_boneIndex, m_newCfg, m_newWind, m_newCursor);
    }
}

void SetPhysicsSpringCommand::undo()
{
    if (const ApplyPhysFn fn = m_applyFn) {
        fn(m_output, m_objectId, m_boneIndex, m_oldCfg, m_oldWind, m_oldCursor);
    }
}

QString SetPhysicsSpringCommand::describe() const
{
    if (m_output.isEmpty()) {
        return QStringLiteral("Adjust physics spring bone %1").arg(m_boneIndex);
    }
    return QStringLiteral("Adjust physics spring bone %1 on %2").arg(m_boneIndex).arg(m_output);
}

bool SetPhysicsSpringCommand::mergeWith(const ICommand *other)
{
    const auto *otherCmd = fastCastCommand<SetPhysicsSpringCommand>(other);
    if (!otherCmd || !qStringsEqualFast(m_output, otherCmd->m_output) ||
        m_objectId != otherCmd->m_objectId || m_boneIndex != otherCmd->m_boneIndex ||
        !withinCoalesceWindowMs(m_timestamp, otherCmd->m_timestamp)) {
        return false;
    }

    m_newCfg = otherCmd->m_newCfg;
    m_newWind = otherCmd->m_newWind;
    m_newCursor = otherCmd->m_newCursor;
    if (otherCmd->m_applyFn) {
        m_applyFn = otherCmd->m_applyFn;
    }
    m_timestamp = otherCmd->m_timestamp;
    return true;
}

size_t SetPhysicsSpringCommand::memoryFootprint() const
{
    return sizeof(*this) + qStringHeapBytes(m_output);
}

} // namespace waywal::studio
