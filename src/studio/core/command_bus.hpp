// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_COMMAND_BUS_HPP
#define WAYWAL_STUDIO_COMMAND_BUS_HPP

#include "services.hpp"
#include "waywal/bezier.h"
#include "waywal/ipc_proto.h"

#include <QObject>
#include <QString>

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace waywal::studio {

/* ========================================================================= */
/* Command Interface                                                         */
/* ========================================================================= */

class ICommand {
public:
    virtual ~ICommand() = default;

    virtual void execute() = 0;
    virtual void undo() = 0;
    virtual void redo() { execute(); }
    virtual QString describe() const = 0;
    virtual bool mergeWith(const ICommand *other) {
        (void)other;
        return false;
    }
};

/* ========================================================================= */
/* Command Bus with 1,000-Step History & Micro-Drag Coalescing               */
/* ========================================================================= */

class CommandBus : public QObject {
    Q_OBJECT

public:
    explicit CommandBus(size_t maxHistory = 1000, QObject *parent = nullptr);
    ~CommandBus() override = default;

    void execute(std::unique_ptr<ICommand> cmd);
    void undo();
    void redo();
    void clear();

    [[nodiscard]] bool canUndo() const;
    [[nodiscard]] bool canRedo() const;
    [[nodiscard]] QString undoText() const;
    [[nodiscard]] QString redoText() const;
    [[nodiscard]] size_t undoCount() const;
    [[nodiscard]] size_t redoCount() const;
    [[nodiscard]] size_t maxHistory() const { return m_maxHistory; }

signals:
    void historyChanged(bool canUndo, bool canRedo, const QString &undoText, const QString &redoText);
    void stateChanged();

private:
    void notifyHistoryChanged();

    std::deque<std::unique_ptr<ICommand>> m_undoStack;
    std::vector<std::unique_ptr<ICommand>> m_redoStack;
    size_t m_maxHistory{1000};
};

/* ========================================================================= */
/* Concrete Reusable Commands                                                */
/* ========================================================================= */

class SetUniformCommand : public ICommand {
public:
    using ApplyFunction = std::function<void(const QString &output,
                                             const QString &name,
                                             const float *vals,
                                             uint32_t count,
                                             uint32_t flags)>;

    SetUniformCommand(QString output,
                      QString uniformName,
                      std::vector<float> oldValues,
                      std::vector<float> newValues,
                      uint32_t flags = 0,
                      ApplyFunction applyFunc = nullptr);

    void execute() override;
    void undo() override;
    [[nodiscard]] QString describe() const override;
    bool mergeWith(const ICommand *other) override;

    [[nodiscard]] const QString &output() const { return m_output; }
    [[nodiscard]] const QString &uniformName() const { return m_uniformName; }
    [[nodiscard]] const std::vector<float> &oldValues() const { return m_oldValues; }
    [[nodiscard]] const std::vector<float> &newValues() const { return m_newValues; }

private:
    QString m_output;
    QString m_uniformName;
    std::vector<float> m_oldValues;
    std::vector<float> m_newValues;
    uint32_t m_flags{0};
    ApplyFunction m_applyFunc;
    std::chrono::steady_clock::time_point m_timestamp;
};

class SetBezierCurveCommand : public ICommand {
public:
    using ApplyFunction = std::function<void(const QString &output, const bezier_curve_t &curve)>;

    SetBezierCurveCommand(QString output,
                          bezier_curve_t oldCurve,
                          bezier_curve_t newCurve,
                          ApplyFunction applyFunc = nullptr);

    void execute() override;
    void undo() override;
    [[nodiscard]] QString describe() const override;
    bool mergeWith(const ICommand *other) override;

    [[nodiscard]] const QString &output() const { return m_output; }
    [[nodiscard]] const bezier_curve_t &oldCurve() const { return m_oldCurve; }
    [[nodiscard]] const bezier_curve_t &newCurve() const { return m_newCurve; }

private:
    QString m_output;
    bezier_curve_t m_oldCurve{};
    bezier_curve_t m_newCurve{};
    ApplyFunction m_applyFunc;
    std::chrono::steady_clock::time_point m_timestamp;
};

class ApplyWallpaperCommand : public ICommand {
public:
    using ApplyFunction = std::function<void(const QString &output,
                                             const waywal_img_metadata_t &meta,
                                             const QString &path)>;

    ApplyWallpaperCommand(QString output,
                          waywal_img_metadata_t oldMeta,
                          waywal_img_metadata_t newMeta,
                          QString oldPath,
                          QString newPath,
                          ApplyFunction applyFunc = nullptr);

    void execute() override;
    void undo() override;
    [[nodiscard]] QString describe() const override;

    [[nodiscard]] const QString &output() const { return m_output; }
    [[nodiscard]] const QString &newPath() const { return m_newPath; }
    [[nodiscard]] const QString &oldPath() const { return m_oldPath; }

private:
    QString m_output;
    waywal_img_metadata_t m_oldMeta{};
    waywal_img_metadata_t m_newMeta{};
    QString m_oldPath;
    QString m_newPath;
    ApplyFunction m_applyFunc;
};

class SetPowerPolicyCommand : public ICommand {
public:
    using ApplyFunction = std::function<void(uint32_t policy)>;

    SetPowerPolicyCommand(uint32_t oldPolicy,
                          uint32_t newPolicy,
                          ApplyFunction applyFunc = nullptr);

    void execute() override;
    void undo() override;
    [[nodiscard]] QString describe() const override;

    [[nodiscard]] uint32_t oldPolicy() const { return m_oldPolicy; }
    [[nodiscard]] uint32_t newPolicy() const { return m_newPolicy; }

private:
    uint32_t m_oldPolicy{0};
    uint32_t m_newPolicy{0};
    ApplyFunction m_applyFunc;
};

} // namespace waywal::studio

#endif // WAYWAL_STUDIO_COMMAND_BUS_HPP
