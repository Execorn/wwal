/**
 * @file lazy_panel_host.hpp
 * @brief Deferred QWidget host that realizes heavyweight panels only when actually revealed.
 */
#pragma once

#include <QDockWidget>
#include <QPaintEvent>
#include <QPointer>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSize>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <functional>
#include <utility>

namespace wwal::studio {

/**
 * @brief Lightweight placeholder container that constructs its child panel on first reveal.
 *
 * Intentionally omits Qt meta-object macros so it remains header-only outside moc lists.
 */
class LazyPanelHost final : public QWidget {
  public:
    using RealizeFn = std::function<QWidget *(QWidget *parent)>;
    using LayoutBusyFn = std::function<bool()>;

    LazyPanelHost(RealizeFn realizeFn, LayoutBusyFn layoutBusyFn = {}, QWidget *parent = nullptr)
        : QWidget(parent), m_realizeFn(std::move(realizeFn)),
          m_layoutBusyFn(std::move(layoutBusyFn))
    {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
    }

    [[nodiscard]] bool isRealized() const noexcept { return m_realizedWidget != nullptr; }
    [[nodiscard]] QWidget *realizedWidget() const noexcept { return m_realizedWidget; }

    [[nodiscard]] bool isActuallyRevealed() const
    {
        if (!isVisible()) {
            return false;
        }
        if (m_layoutBusyFn && m_layoutBusyFn()) {
            return false;
        }
        const QWidget *win = window();
        if (!win || !win->isVisible()) {
            return false;
        }
        if (width() <= 0 || height() <= 0) {
            return false;
        }
        if (const QDockWidget *dock = findEnclosingDock()) {
            if (!dock->isVisible() || dock->isHidden()) {
                return false;
            }
            // Qt QMainWindowLayout parks inactive tabified docks at negative coordinates while
            // keeping isVisible() true; ignore offscreen parked docks until tab activation.
            if (!dock->isFloating() &&
                (dock->x() < 0 || dock->y() < 0 || dock->width() <= 0 || dock->height() <= 0)) {
                return false;
            }
        }
        return true;
    }

    QWidget *realizeNow()
    {
        if (m_realizedWidget || !m_realizeFn) {
            return m_realizedWidget;
        }
        auto fn = std::exchange(m_realizeFn, {});
        QWidget *created = fn(this);
        if (created) {
            m_realizedWidget = created;
            if (auto *vbox = qobject_cast<QVBoxLayout *>(layout())) {
                vbox->addWidget(created);
            }
            created->show();
        }
        return m_realizedWidget;
    }

    QWidget *realizeIfRevealed()
    {
        if (m_realizedWidget) {
            return m_realizedWidget;
        }
        if (!isActuallyRevealed()) {
            return nullptr;
        }
        return realizeNow();
    }

    QSize sizeHint() const override
    {
        return m_realizedWidget ? m_realizedWidget->sizeHint() : QSize(240, 180);
    }

  protected:
    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        if (!m_realizedWidget) {
            // Defer until post-show layout geometry settles so parked background tabs stay lazy.
            scheduleRevealCheck();
        }
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        if (!m_realizedWidget && isVisible()) {
            scheduleRevealCheck();
        }
    }

    void paintEvent(QPaintEvent *event) override
    {
        if (!m_realizedWidget && isActuallyRevealed()) {
            realizeNow();
        }
        QWidget::paintEvent(event);
    }

  private:
    [[nodiscard]] const QDockWidget *findEnclosingDock() const
    {
        for (const QWidget *p = parentWidget(); p != nullptr; p = p->parentWidget()) {
            if (const auto *dock = qobject_cast<const QDockWidget *>(p)) {
                return dock;
            }
        }
        return nullptr;
    }

    void scheduleRevealCheck()
    {
        if (m_revealCheckPending || m_realizedWidget) {
            return;
        }
        m_revealCheckPending = true;
        QTimer::singleShot(0, this, [this]() {
            m_revealCheckPending = false;
            realizeIfRevealed();
        });
    }

    RealizeFn m_realizeFn;
    LayoutBusyFn m_layoutBusyFn;
    QWidget *m_realizedWidget{nullptr};
    bool m_revealCheckPending{false};
};

} // namespace wwal::studio
