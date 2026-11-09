// SPDX-License-Identifier: GPL-3.0-or-later
#include "studio/app/dock_layout_manager.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QDebug>
#include <QStyle>
#include <QApplication>
#include <QSplitter>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QEvent>
#include <algorithm>
#include <cmath>

namespace waywal::studio {

/* ========================================================================= */
/* StudioDockTitleBar Implementation                                         */
/* ========================================================================= */

StudioDockTitleBar::StudioDockTitleBar(const QString &title, QDockWidget *parentDock,
                                       QWidget *parent)
    : QWidget(parent), m_parentDock(parentDock)
{
    setObjectName(QStringLiteral("StudioDockTitleBar"));
    setFixedHeight(28);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 2, 8, 2);
    layout->setSpacing(8);

    // Title label
    m_titleLabel = new QLabel(title, this);
    m_titleLabel->setObjectName(QStringLiteral("DockTitleText"));
    m_titleLabel->setStyleSheet(
        QStringLiteral("color: #e2e8f0; font-size: 11px; font-weight: 600;"));
    layout->addWidget(m_titleLabel, 1);

    // Float / Detach button
    m_floatButton = new QToolButton(this);
    m_floatButton->setObjectName(QStringLiteral("DockFloatButton"));
    m_floatButton->setToolTip(QStringLiteral("Detach into floating window"));
    m_floatButton->setText(QStringLiteral("Float"));
    m_floatButton->setFixedHeight(20);
    m_floatButton->setStyleSheet(QStringLiteral(
        "QToolButton { background: transparent; border: none; color: #94a3b8; font-size: 10px; "
        "padding: 2px 6px; border-radius: 3px; }"
        "QToolButton:hover { background: #262c3a; color: #f8fafc; }"));
    connect(m_floatButton, &QToolButton::clicked, this, [this]() {
        if (m_parentDock) {
            m_parentDock->setFloating(!m_parentDock->isFloating());
        }
        emit floatToggleRequested();
    });
    layout->addWidget(m_floatButton);

    // Close button
    m_closeButton = new QToolButton(this);
    m_closeButton->setObjectName(QStringLiteral("DockCloseButton"));
    m_closeButton->setToolTip(QStringLiteral("Hide panel"));
    m_closeButton->setText(QStringLiteral("Hide"));
    m_closeButton->setFixedHeight(20);
    m_closeButton->setStyleSheet(QStringLiteral(
        "QToolButton { background: transparent; border: none; color: #94a3b8; font-size: 10px; "
        "padding: 2px 6px; border-radius: 3px; }"
        "QToolButton:hover { background: #7f1d1d; color: #ffffff; }"));
    connect(m_closeButton, &QToolButton::clicked, this, [this]() {
        if (m_parentDock) {
            m_parentDock->close();
        }
        emit closeRequested();
    });
    layout->addWidget(m_closeButton);

    setStyleSheet(QStringLiteral("#StudioDockTitleBar {"
                                 "  background: #181b22;"
                                 "  border-bottom: 1px solid #262b36;"
                                 "}"));
}

void StudioDockTitleBar::setTitle(const QString &title)
{
    if (m_titleLabel) {
        m_titleLabel->setText(title);
    }
}

QString StudioDockTitleBar::title() const
{
    return m_titleLabel ? m_titleLabel->text() : QString();
}

void StudioDockTitleBar::updateFloatingState(bool floating)
{
    if (m_floatButton) {
        if (floating) {
            m_floatButton->setText(QStringLiteral("Dock"));
            m_floatButton->setToolTip(QStringLiteral("Return panel to main window"));
        } else {
            m_floatButton->setText(QStringLiteral("Float"));
            m_floatButton->setToolTip(QStringLiteral("Detach into floating window"));
        }
    }
}

void StudioDockTitleBar::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_parentDock) {
        m_parentDock->setFloating(!m_parentDock->isFloating());
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

/* ========================================================================= */
/* StudioDockWidget Implementation                                           */
/* ========================================================================= */

StudioDockWidget::StudioDockWidget(const QString &id, const QString &title, QWidget *content,
                                   QWidget *parent)
    : QDockWidget(QString(title).replace(QLatin1String("&"), QLatin1String("&&")), parent),
      m_id(id), m_content(content)
{
    setObjectName(id);
    setAllowedAreas(Qt::AllDockWidgetAreas);
    setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable |
                QDockWidget::DockWidgetClosable);

    if (m_content) {
        setWidget(m_content);
    }

    m_customTitleBar = new StudioDockTitleBar(title, this, this);
    setTitleBarWidget(m_customTitleBar);

    connect(this, &QDockWidget::topLevelChanged, this, &StudioDockWidget::onTopLevelChanged);
}

void StudioDockWidget::setTitleText(const QString &title)
{
    setWindowTitle(QString(title).replace(QLatin1String("&"), QLatin1String("&&")));
    if (m_customTitleBar) {
        m_customTitleBar->setTitle(title);
    }
}

void StudioDockWidget::setChromelessBarMode(bool chromeless)
{
    if (chromeless) {
        auto *emptyTitle = new QWidget(this);
        emptyTitle->setFixedHeight(0);
        setTitleBarWidget(emptyTitle);
        setFeatures(QDockWidget::NoDockWidgetFeatures);
    } else if (m_customTitleBar) {
        setTitleBarWidget(m_customTitleBar);
        setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable |
                    QDockWidget::DockWidgetClosable);
    }
}

void StudioDockWidget::closeEvent(QCloseEvent *event) { QDockWidget::closeEvent(event); }

void StudioDockWidget::onTopLevelChanged(bool floating)
{
    if (m_customTitleBar) {
        m_customTitleBar->updateFloatingState(floating);
    }
}

/* ========================================================================= */
/* DockLayoutManager Implementation                                          */
/* ========================================================================= */

DockLayoutManager::DockLayoutManager(QMainWindow *mainWindow, QObject *parent)
    : QObject(parent), m_mainWindow(mainWindow)
{
    if (m_mainWindow) {
        // Enable flexible docking features
        m_mainWindow->setDockNestingEnabled(true);
        m_mainWindow->setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks |
                                     QMainWindow::AllowTabbedDocks | QMainWindow::GroupedDragging);

        // Ensure Top (Topology) and Bottom (Footer) bars span full window width
        m_mainWindow->setCorner(Qt::TopLeftCorner, Qt::TopDockWidgetArea);
        m_mainWindow->setCorner(Qt::TopRightCorner, Qt::TopDockWidgetArea);
        m_mainWindow->setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
        m_mainWindow->setCorner(Qt::BottomRightCorner, Qt::BottomDockWidgetArea);

        m_mainWindow->installEventFilter(this);
    }
    if (qApp) {
        qApp->installEventFilter(this);
    }

#ifdef HAVE_QT_ADS
    ads::CDockManager::setConfigFlag(ads::CDockManager::OpaqueSplitterResize, true);
    ads::CDockManager::setConfigFlag(ads::CDockManager::XmlCompressionEnabled, false);
    ads::CDockManager::setConfigFlag(ads::CDockManager::FocusHighlighting, true);
    m_adsManager = new ads::CDockManager(m_mainWindow);
#endif

    // Setup bidirectional panel aliases
    m_aliases.insert(QStringLiteral("Wallpapers"), QStringLiteral("Wallpapers"));
    m_aliases.insert(QStringLiteral("VirtualizedGridWidget"), QStringLiteral("Wallpapers"));
    m_aliases.insert(QStringLiteral("Grid"), QStringLiteral("Wallpapers"));

    m_aliases.insert(QStringLiteral("GpuPreviewViewport"), QStringLiteral("GpuPreviewViewport"));
    m_aliases.insert(QStringLiteral("GpuViewport"), QStringLiteral("GpuPreviewViewport"));
    m_aliases.insert(QStringLiteral("Viewport"), QStringLiteral("GpuPreviewViewport"));

    m_aliases.insert(QStringLiteral("CubicBezier"), QStringLiteral("CubicBezier"));
    m_aliases.insert(QStringLiteral("CubicBezierEditorWidget"), QStringLiteral("CubicBezier"));
    m_aliases.insert(QStringLiteral("BezierCurves"), QStringLiteral("CubicBezier"));

    m_aliases.insert(QStringLiteral("PropertyInspectorWidget"),
                     QStringLiteral("PropertyInspectorWidget"));
    m_aliases.insert(QStringLiteral("PropertyInspector"),
                     QStringLiteral("PropertyInspectorWidget"));
    m_aliases.insert(QStringLiteral("Inspector"), QStringLiteral("PropertyInspectorWidget"));

    m_aliases.insert(QStringLiteral("ThemeStudioWidget"), QStringLiteral("ThemeStudioWidget"));
    m_aliases.insert(QStringLiteral("ThemeStudio"), QStringLiteral("ThemeStudioWidget"));
    m_aliases.insert(QStringLiteral("Theme"), QStringLiteral("ThemeStudioWidget"));

    m_aliases.insert(QStringLiteral("MonitorTopologyBar"), QStringLiteral("MonitorTopologyBar"));
    m_aliases.insert(QStringLiteral("MonitorTopologyCanvas"), QStringLiteral("MonitorTopologyBar"));
    m_aliases.insert(QStringLiteral("Topology"), QStringLiteral("MonitorTopologyBar"));

    m_aliases.insert(QStringLiteral("AudioFFT"), QStringLiteral("AudioFFT"));
    m_aliases.insert(QStringLiteral("AudioSpectrumWidget"), QStringLiteral("AudioFFT"));

    m_aliases.insert(QStringLiteral("PowerGovernor"), QStringLiteral("PowerGovernor"));
    m_aliases.insert(QStringLiteral("PowerGovernorWidget"), QStringLiteral("PowerGovernor"));

    m_aliases.insert(QStringLiteral("Telemetry"), QStringLiteral("Telemetry"));
    m_aliases.insert(QStringLiteral("TelemetryHudWidget"), QStringLiteral("Telemetry"));

    m_aliases.insert(QStringLiteral("StudioFooterBar"), QStringLiteral("StudioFooterBar"));
    m_aliases.insert(QStringLiteral("Footer"), QStringLiteral("StudioFooterBar"));

    // Populate built-in perspectives
    setupDefaultPerspectives();
}

DockLayoutManager::~DockLayoutManager()
{
    if (qApp) {
        qApp->removeEventFilter(this);
    }
#ifdef HAVE_QT_ADS
    delete m_adsManager;
    m_adsManager = nullptr;
#endif
}

bool DockLayoutManager::eventFilter(QObject *watched, QEvent *event)
{
    if (!m_applyingLayout && m_mainWindow) {
        if (watched == m_mainWindow &&
            (event->type() == QEvent::Show || event->type() == QEvent::Resize)) {
            m_applyingLayout = true;
            applyCurrentSplitSizes();
            m_applyingLayout = false;
            QTimer::singleShot(0, this, [this]() {
                if (!m_applyingLayout) {
                    m_applyingLayout = true;
                    applyCurrentSplitSizes();
                    m_applyingLayout = false;
                }
            });
        } else if (event->type() == QEvent::MouseButtonRelease && m_mainWindow->isVisible() &&
                   m_perspectives.contains(m_activePerspective)) {
            const bool isWithinMainWindow =
                (watched == m_mainWindow) ||
                (watched && watched->isWidgetType() &&
                 m_mainWindow->isAncestorOf(static_cast<QWidget *>(watched)));
            if (isWithinMainWindow) {
                QJsonObject tree = m_perspectives.value(m_activePerspective);
                syncLiveRatiosFromGeometry(tree);
                m_perspectives.insert(m_activePerspective, tree);
                QTimer::singleShot(0, this, [this]() {
                    if (!m_applyingLayout && m_mainWindow && m_mainWindow->isVisible() &&
                        m_perspectives.contains(m_activePerspective)) {
                        QJsonObject delayedTree = m_perspectives.value(m_activePerspective);
                        syncLiveRatiosFromGeometry(delayedTree);
                        m_perspectives.insert(m_activePerspective, delayedTree);
                    }
                });
            }
        }
    }
    return QObject::eventFilter(watched, event);
}

void DockLayoutManager::setupDefaultPerspectives()
{
    m_perspectives.insert(QStringLiteral("Default"), createDefaultPerspectiveTree());
    m_perspectives.insert(QStringLiteral("Shader Coding"), createShaderCodingPerspectiveTree());
    m_perspectives.insert(QStringLiteral("Theme Studio"), createThemeStudioPerspectiveTree());
    m_perspectives.insert(QStringLiteral("Multi-Monitor"), createMultiMonitorPerspectiveTree());
}

QString DockLayoutManager::resolvePanelId(const QString &idOrAlias) const
{
    if (m_docks.contains(idOrAlias)) {
        return idOrAlias;
    }
    if (m_aliases.contains(idOrAlias)) {
        QString mapped = m_aliases.value(idOrAlias);
        if (m_docks.contains(mapped)) {
            return mapped;
        }
    }
    // Case-insensitive fallback
    for (auto it = m_docks.constBegin(); it != m_docks.constEnd(); ++it) {
        if (it.key().compare(idOrAlias, Qt::CaseInsensitive) == 0) {
            return it.key();
        }
    }
    return idOrAlias;
}

StudioDockWidget *DockLayoutManager::resolveDock(const QString &idOrAlias) const
{
    QString canonical = resolvePanelId(idOrAlias);
    return m_docks.value(canonical, nullptr);
}

void DockLayoutManager::registerPanel(const QString &id, QWidget *widget, const QString &title,
                                      Qt::DockWidgetArea defaultArea)
{
    if (m_docks.contains(id)) {
        return;
    }

    auto *dock = new StudioDockWidget(id, title, widget, m_mainWindow);
    if (id == QStringLiteral("StudioFooterBar")) {
        dock->setChromelessBarMode(true);
        dock->setAllowedAreas(Qt::BottomDockWidgetArea);
    }
    m_docks.insert(id, dock);

    if (m_mainWindow) {
        m_mainWindow->addDockWidget(defaultArea, dock);
    }

#ifdef HAVE_QT_ADS
    if (m_adsManager) {
        auto *adsDock = new ads::CDockWidget(title, m_mainWindow);
        adsDock->setObjectName(id);
        if (widget) {
            adsDock->setWidget(widget);
        }
        ads::DockWidgetArea adsArea = ads::LeftDockWidgetArea;
        if (defaultArea == Qt::RightDockWidgetArea)
            adsArea = ads::RightDockWidgetArea;
        else if (defaultArea == Qt::TopDockWidgetArea)
            adsArea = ads::TopDockWidgetArea;
        else if (defaultArea == Qt::BottomDockWidgetArea)
            adsArea = ads::BottomDockWidgetArea;
        m_adsManager->addDockWidget(adsArea, adsDock);
        m_adsDocks.insert(id, adsDock);
    }
#endif

    connect(dock, &QDockWidget::visibilityChanged, this, [this, id, dock](bool /*visible*/) {
        if (!m_applyingLayout) {
            emit panelVisibilityChanged(id, !dock->isHidden());
            emit layoutChanged();
        }
    });

    emit layoutChanged();
}

void DockLayoutManager::unregisterPanel(const QString &id)
{
    QString canonical = resolvePanelId(id);
    if (!m_docks.contains(canonical)) {
        return;
    }

    auto *dock = m_docks.take(canonical);
    if (m_mainWindow && dock) {
        m_mainWindow->removeDockWidget(dock);
    }
    delete dock;

#ifdef HAVE_QT_ADS
    if (m_adsDocks.contains(canonical)) {
        auto *adsDock = m_adsDocks.take(canonical);
        delete adsDock;
    }
#endif

    emit layoutChanged();
}

StudioDockWidget *DockLayoutManager::dockWidget(const QString &id) const { return resolveDock(id); }

QWidget *DockLayoutManager::panelWidget(const QString &id) const
{
    auto *dock = resolveDock(id);
    return dock ? dock->contentWidget() : nullptr;
}

QStringList DockLayoutManager::registeredPanelIds() const { return m_docks.keys(); }

void DockLayoutManager::setPanelVisible(const QString &id, bool visible)
{
    auto *dock = resolveDock(id);
    if (!dock)
        return;

    if (visible) {
        dock->lower();
        dock->show();
        dock->raise();
        if (!m_applyingLayout && m_mainWindow && m_mainWindow->isVisible() &&
            m_perspectives.contains(m_activePerspective)) {
            if (m_mainWindow->layout()) {
                m_mainWindow->layout()->activate();
            }
            QJsonObject tree = m_perspectives.value(m_activePerspective);
            syncLiveRatiosFromGeometry(tree);
            m_perspectives.insert(m_activePerspective, tree);
        }
    } else {
        dock->hide();
    }
    emit panelVisibilityChanged(dock->panelId(), visible);
    emit layoutChanged();
}

bool DockLayoutManager::isPanelVisible(const QString &id) const
{
    auto *dock = resolveDock(id);
    return dock ? dock->isVisible() : false;
}

void DockLayoutManager::togglePanelVisibility(const QString &id)
{
    setPanelVisible(id, !isPanelVisible(id));
}

void DockLayoutManager::setPanelFloating(const QString &id, bool floating)
{
    auto *dock = resolveDock(id);
    if (dock) {
        dock->setFloating(floating);
        if (floating) {
            dock->show();
            dock->raise();
        }
        emit layoutChanged();
    }
}

bool DockLayoutManager::isPanelFloating(const QString &id) const
{
    auto *dock = resolveDock(id);
    return dock ? dock->isFloating() : false;
}

QStringList DockLayoutManager::availablePerspectives() const { return m_perspectives.keys(); }

QString DockLayoutManager::activePerspective() const { return m_activePerspective; }

bool DockLayoutManager::switchPerspective(const QString &name)
{
    if (!m_perspectives.contains(name)) {
        if (name == QStringLiteral("Default")) {
            m_perspectives.insert(name, createDefaultPerspectiveTree());
        } else if (name == QStringLiteral("Shader Coding")) {
            m_perspectives.insert(name, createShaderCodingPerspectiveTree());
        } else if (name == QStringLiteral("Theme Studio")) {
            m_perspectives.insert(name, createThemeStudioPerspectiveTree());
        } else if (name == QStringLiteral("Multi-Monitor")) {
            m_perspectives.insert(name, createMultiMonitorPerspectiveTree());
        } else {
            return false;
        }
    }

    m_activePerspective = name;
    applyPerspectiveTree(m_perspectives.value(name));

    emit perspectiveChanged(name);
    emit layoutChanged();
    return true;
}

void DockLayoutManager::savePerspective(const QString &name)
{
    m_perspectives.insert(name, serializePerspectiveTree(name));
    emit layoutChanged();
}

void DockLayoutManager::removePerspective(const QString &name)
{
    // Do not delete default perspectives
    if (name == QStringLiteral("Default"))
        return;
    if (m_perspectives.remove(name) > 0) {
        emit layoutChanged();
    }
}

void DockLayoutManager::resetToDefaultPerspective()
{
    setupDefaultPerspectives();
    switchPerspective(QStringLiteral("Default"));
}

/* ========================================================================= */
/* Layout Persistence & Tree Algorithms                                      */
/* ========================================================================= */

void DockLayoutManager::collectDocksInTree(const QJsonObject &node, QSet<QString> &outIds)
{
    if (node.contains(QStringLiteral("type"))) {
        QString type = node.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("DockNode")) {
            QString content = node.value(QStringLiteral("content")).toString();
            QString resolved = resolvePanelId(content);
            if (!resolved.isEmpty())
                outIds.insert(resolved);
        } else if (type == QStringLiteral("TabGroup")) {
            QJsonArray tabs = node.value(QStringLiteral("tabs")).toArray();
            for (const auto &tabVal : tabs) {
                QString resolved = resolvePanelId(tabVal.toString());
                if (!resolved.isEmpty())
                    outIds.insert(resolved);
            }
        }
    }

    if (node.contains(QStringLiteral("split_direction"))) {
        QString dir = node.value(QStringLiteral("split_direction")).toString();
        if (dir.compare(QStringLiteral("Horizontal"), Qt::CaseInsensitive) == 0) {
            collectDocksInTree(node.value(QStringLiteral("left")).toObject(), outIds);
            collectDocksInTree(node.value(QStringLiteral("right")).toObject(), outIds);
        } else {
            collectDocksInTree(node.value(QStringLiteral("top")).toObject(), outIds);
            collectDocksInTree(node.value(QStringLiteral("bottom")).toObject(), outIds);
        }
    }
}

StudioDockWidget *DockLayoutManager::findPrimaryDock(const QJsonObject &node) const
{
    if (node.isEmpty())
        return nullptr;

    QString type = node.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("DockNode")) {
        return resolveDock(node.value(QStringLiteral("content")).toString());
    }
    if (type == QStringLiteral("TabGroup")) {
        QJsonArray tabs = node.value(QStringLiteral("tabs")).toArray();
        for (const auto &tabVal : tabs) {
            if (auto *dock = resolveDock(tabVal.toString())) {
                if (!dock->isHidden() && dock->x() >= 0 && dock->y() >= 0 &&
                    (dock->x() > 0 || dock->y() > 0)) {
                    return dock;
                }
            }
        }
        QString activeTab = node.value(QStringLiteral("active_tab")).toString();
        if (!activeTab.isEmpty()) {
            if (auto *activeDock = resolveDock(activeTab)) {
                if (!activeDock->isHidden() && activeDock->x() >= 0 && activeDock->y() >= 0) {
                    return activeDock;
                }
            }
        }
        for (const auto &tabVal : tabs) {
            if (auto *dock = resolveDock(tabVal.toString())) {
                return dock;
            }
        }
        return nullptr;
    }
    if (node.contains(QStringLiteral("split_direction"))) {
        QString dir = node.value(QStringLiteral("split_direction")).toString();
        bool horiz = (dir.compare(QStringLiteral("Horizontal"), Qt::CaseInsensitive) == 0);
        QJsonObject firstChild = horiz ? node.value(QStringLiteral("left")).toObject()
                                       : node.value(QStringLiteral("top")).toObject();
        QJsonObject secondChild = horiz ? node.value(QStringLiteral("right")).toObject()
                                        : node.value(QStringLiteral("bottom")).toObject();
        if (auto *d1 = findPrimaryDock(firstChild))
            return d1;
        return findPrimaryDock(secondChild);
    }
    return nullptr;
}

void DockLayoutManager::buildSubtreeTopDown(const QJsonObject &node,
                                            StudioDockWidget *currentPrimary)
{
    if (node.isEmpty() || !m_mainWindow || !currentPrimary)
        return;

    QString type = node.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("DockNode")) {
        currentPrimary->setFloating(false);
        currentPrimary->show();
        return;
    }

    if (type == QStringLiteral("TabGroup")) {
        QJsonArray tabs = node.value(QStringLiteral("tabs")).toArray();
        QString activeTab = node.value(QStringLiteral("active_tab")).toString();

        currentPrimary->setFloating(false);
        currentPrimary->show();

        for (const auto &tabVal : tabs) {
            auto *dock = resolveDock(tabVal.toString());
            if (!dock || dock == currentPrimary)
                continue;

            dock->setFloating(false);
            m_mainWindow->tabifyDockWidget(currentPrimary, dock);
            dock->show();
        }

        if (!activeTab.isEmpty()) {
            if (auto *active = resolveDock(activeTab)) {
                active->lower();
                active->show();
                active->raise();
                return;
            }
        }
        currentPrimary->lower();
        currentPrimary->show();
        currentPrimary->raise();
        return;
    }

    if (node.contains(QStringLiteral("split_direction"))) {
        QString dir = node.value(QStringLiteral("split_direction")).toString();
        Qt::Orientation orient =
            (dir.compare(QStringLiteral("Horizontal"), Qt::CaseInsensitive) == 0) ? Qt::Horizontal
                                                                                  : Qt::Vertical;

        QJsonObject firstChild = (orient == Qt::Horizontal)
                                     ? node.value(QStringLiteral("left")).toObject()
                                     : node.value(QStringLiteral("top")).toObject();
        QJsonObject secondChild = (orient == Qt::Horizontal)
                                      ? node.value(QStringLiteral("right")).toObject()
                                      : node.value(QStringLiteral("bottom")).toObject();

        StudioDockWidget *firstPrimary = findPrimaryDock(firstChild);
        StudioDockWidget *secondPrimary = findPrimaryDock(secondChild);

        if (firstPrimary && secondPrimary && firstPrimary != secondPrimary) {
            secondPrimary->setFloating(false);
            m_mainWindow->splitDockWidget(firstPrimary, secondPrimary, orient);
            firstPrimary->show();
            secondPrimary->show();

            buildSubtreeTopDown(firstChild, firstPrimary);
            buildSubtreeTopDown(secondChild, secondPrimary);
        } else if (firstPrimary) {
            buildSubtreeTopDown(firstChild, firstPrimary);
        } else if (secondPrimary) {
            buildSubtreeTopDown(secondChild, secondPrimary);
        }
    }
}

void DockLayoutManager::applySubtreeSizes(const QJsonObject &node, int availW, int availH,
                                          QList<QDockWidget *> &horizDocks, QList<int> &horizSizes)
{
    if (node.isEmpty() || !m_mainWindow)
        return;

    if (!node.contains(QStringLiteral("split_direction"))) {
        if (auto *leafDock = findPrimaryDock(node)) {
            if (!horizDocks.contains(leafDock)) {
                horizDocks.append(leafDock);
                horizSizes.append(std::max(160, availW));
            }
        }
        return;
    }

    QString dir = node.value(QStringLiteral("split_direction")).toString();
    Qt::Orientation orient = (dir.compare(QStringLiteral("Horizontal"), Qt::CaseInsensitive) == 0)
                                 ? Qt::Horizontal
                                 : Qt::Vertical;

    QJsonObject firstChild = (orient == Qt::Horizontal)
                                 ? node.value(QStringLiteral("left")).toObject()
                                 : node.value(QStringLiteral("top")).toObject();
    QJsonObject secondChild = (orient == Qt::Horizontal)
                                  ? node.value(QStringLiteral("right")).toObject()
                                  : node.value(QStringLiteral("bottom")).toObject();

    double ratio = std::clamp(node.value(QStringLiteral("split_ratio")).toDouble(0.5), 0.15, 0.85);
    StudioDockWidget *firstPrimary = findPrimaryDock(firstChild);
    StudioDockWidget *secondPrimary = findPrimaryDock(secondChild);

    if (orient == Qt::Horizontal) {
        int w1 = std::max(180, static_cast<int>(std::round(availW * ratio)));
        int w2 = std::max(180, availW - w1);
        if (firstPrimary && secondPrimary && firstPrimary != secondPrimary) {
            m_mainWindow->resizeDocks({firstPrimary, secondPrimary}, {w1, w2}, Qt::Horizontal);
        }
        applySubtreeSizes(firstChild, w1, availH, horizDocks, horizSizes);
        applySubtreeSizes(secondChild, w2, availH, horizDocks, horizSizes);
    } else {
        int h1 = std::max(120, static_cast<int>(std::round(availH * ratio)));
        int h2 = std::max(120, availH - h1);
        if (firstPrimary && secondPrimary && firstPrimary != secondPrimary) {
            m_mainWindow->resizeDocks({firstPrimary, secondPrimary}, {h1, h2}, Qt::Vertical);
        }
        QList<QDockWidget *> topHorizDocks;
        QList<int> topHorizSizes;
        applySubtreeSizes(firstChild, availW, h1, topHorizDocks, topHorizSizes);

        QList<QDockWidget *> bottomHorizDocks;
        QList<int> bottomHorizSizes;
        applySubtreeSizes(secondChild, availW, h2, bottomHorizDocks, bottomHorizSizes);

        if (topHorizDocks.size() >= bottomHorizDocks.size()) {
            for (int i = 0; i < topHorizDocks.size(); ++i) {
                if (!horizDocks.contains(topHorizDocks[i])) {
                    horizDocks.append(topHorizDocks[i]);
                    horizSizes.append(topHorizSizes[i]);
                }
            }
        } else {
            for (int i = 0; i < bottomHorizDocks.size(); ++i) {
                if (!horizDocks.contains(bottomHorizDocks[i])) {
                    horizDocks.append(bottomHorizDocks[i]);
                    horizSizes.append(bottomHorizSizes[i]);
                }
            }
        }
    }
}

int DockLayoutManager::computeSubtreeSpan(const QJsonObject &node, Qt::Orientation orient) const
{
    if (node.isEmpty())
        return 0;

    if (!node.contains(QStringLiteral("split_direction"))) {
        StudioDockWidget *dock = findPrimaryDock(node);
        if (!dock || dock->isHidden() || dock->x() < 0 || dock->y() < 0) {
            return 0;
        }
        return (orient == Qt::Horizontal) ? dock->width() : dock->height();
    }

    QString dir = node.value(QStringLiteral("split_direction")).toString();
    Qt::Orientation nodeOrient =
        (dir.compare(QStringLiteral("Horizontal"), Qt::CaseInsensitive) == 0) ? Qt::Horizontal
                                                                              : Qt::Vertical;

    QString firstKey =
        (nodeOrient == Qt::Horizontal) ? QStringLiteral("left") : QStringLiteral("top");
    QString secondKey =
        (nodeOrient == Qt::Horizontal) ? QStringLiteral("right") : QStringLiteral("bottom");

    int span1 = computeSubtreeSpan(node.value(firstKey).toObject(), orient);
    int span2 = computeSubtreeSpan(node.value(secondKey).toObject(), orient);

    if (nodeOrient == orient) {
        if (span1 <= 0 || span2 <= 0)
            return 0;
        return span1 + span2;
    }
    return std::max(span1, span2);
}

void DockLayoutManager::syncLiveRatiosFromGeometry(QJsonObject &node) const
{
    if (node.isEmpty())
        return;

    if (node.value(QStringLiteral("type")).toString() == QStringLiteral("TabGroup")) {
        QJsonArray tabs = node.value(QStringLiteral("tabs")).toArray();
        for (const auto &tabVal : tabs) {
            if (auto *dock = resolveDock(tabVal.toString())) {
                if (!dock->isHidden() && dock->x() >= 0 && dock->y() >= 0 &&
                    (dock->x() > 0 || dock->y() > 0)) {
                    node.insert(QStringLiteral("active_tab"), dock->panelId());
                    break;
                }
            }
        }
        return;
    }

    if (!node.contains(QStringLiteral("split_direction")))
        return;

    QString dir = node.value(QStringLiteral("split_direction")).toString();
    Qt::Orientation orient = (dir.compare(QStringLiteral("Horizontal"), Qt::CaseInsensitive) == 0)
                                 ? Qt::Horizontal
                                 : Qt::Vertical;

    QString firstKey = (orient == Qt::Horizontal) ? QStringLiteral("left") : QStringLiteral("top");
    QString secondKey =
        (orient == Qt::Horizontal) ? QStringLiteral("right") : QStringLiteral("bottom");

    QJsonObject firstChild = node.value(firstKey).toObject();
    QJsonObject secondChild = node.value(secondKey).toObject();

    int s1 = computeSubtreeSpan(firstChild, orient);
    int s2 = computeSubtreeSpan(secondChild, orient);
    if (s1 > 40 && s2 > 40 && (s1 + s2) > 100) {
        double liveRatio =
            std::clamp(static_cast<double>(s1) / static_cast<double>(s1 + s2), 0.15, 0.85);
        node.insert(QStringLiteral("split_ratio"), std::round(liveRatio * 1000.0) / 1000.0);
    }

    syncLiveRatiosFromGeometry(firstChild);
    syncLiveRatiosFromGeometry(secondChild);
    node.insert(firstKey, firstChild);
    node.insert(secondKey, secondChild);
}

void DockLayoutManager::applyCurrentSplitSizes()
{
    if (!m_mainWindow || !m_perspectives.contains(m_activePerspective))
        return;

    QJsonObject tree = m_perspectives.value(m_activePerspective);
    if (tree.isEmpty())
        return;

    int totalW = (m_mainWindow->width() >= 640) ? m_mainWindow->width() : 1024;
    int totalH = ((m_mainWindow->height() >= 480) ? m_mainWindow->height() : 720) - 60;

    auto *footerDock = resolveDock(QStringLiteral("StudioFooterBar"));
    if (footerDock && !footerDock->isHidden() && !footerDock->isFloating()) {
        m_mainWindow->resizeDocks({footerDock}, {52}, Qt::Vertical);
        totalH -= 52;
    }

    QSet<QString> treeIds;
    collectDocksInTree(tree, treeIds);

    auto *topDock = resolveDock(QStringLiteral("MonitorTopologyBar"));
    if (topDock && !topDock->isHidden() && !topDock->isFloating() &&
        !treeIds.contains(topDock->panelId())) {
        m_mainWindow->resizeDocks({topDock}, {100}, Qt::Vertical);
        totalH -= 100;
    }

    totalH = std::max(320, totalH);

    QList<QDockWidget *> horizDocks;
    QList<int> horizSizes;
    applySubtreeSizes(tree, totalW, totalH, horizDocks, horizSizes);

    if (horizDocks.size() >= 2) {
        m_mainWindow->resizeDocks(horizDocks, horizSizes, Qt::Horizontal);
    }

    // Before show() (!isVisible()), only hide single-tab/empty QTabBars (count <= 1) and
    // NEVER hide multi-tab QTabBars when y() == 0 before layout.
    // When visible, ensure multi-tab QTabBars are shown before activating QMainWindowLayout
    // so QDockAreaLayout computes their true dock coordinates, then hide any retired
    // unusedTabBars that QMainWindowLayout left parked at y() < 45.
    const auto tabBars =
        m_mainWindow->findChildren<QTabBar *>(QString(), Qt::FindDirectChildrenOnly);
    if (m_mainWindow->isVisible() && m_mainWindow->layout()) {
        for (QTabBar *tb : tabBars) {
            if (tb && tb->count() >= 2) {
                tb->show();
            }
        }
        m_mainWindow->layout()->activate();
        for (QTabBar *tb : tabBars) {
            if (!tb)
                continue;
            if (tb->count() <= 1 || tb->y() < 45) {
                tb->hide();
            } else {
                tb->show();
            }
        }
    } else {
        for (QTabBar *tb : tabBars) {
            if (!tb)
                continue;
            if (tb->count() <= 1) {
                tb->hide();
            }
        }
    }
}

QDockWidget *DockLayoutManager::layoutSubtree(const QJsonObject &node,
                                              Qt::Orientation /*parentOrientation*/)
{
    StudioDockWidget *primary = findPrimaryDock(node);
    if (primary) {
        buildSubtreeTopDown(node, primary);
    }
    return primary;
}

void DockLayoutManager::applyPerspectiveTree(const QJsonObject &tree)
{
    if (!m_mainWindow || tree.isEmpty())
        return;

    m_applyingLayout = true;

    // 1. Collect all panels included in the target perspective tree
    QSet<QString> activeDockIds;
    collectDocksInTree(tree, activeDockIds);

    // 2. Detach all docks cleanly so QMainWindow's internal QDockAreaLayout starts fresh
    for (auto *dock : m_docks) {
        dock->setFloating(false);
        m_mainWindow->removeDockWidget(dock);
        dock->hide();
    }

    // 3. Anchor Top Topology Bar in Default perspective (or when not embedded in tree)
    auto *topDock = resolveDock(QStringLiteral("MonitorTopologyBar"));
    if (topDock && !activeDockIds.contains(topDock->panelId())) {
        if (m_activePerspective == QStringLiteral("Default")) {
            m_mainWindow->addDockWidget(Qt::TopDockWidgetArea, topDock);
            topDock->show();
            activeDockIds.insert(topDock->panelId());
        }
    }

    // 4. Construct main workspace tree top-down in a single unified dock area
    StudioDockWidget *rootPrimary = findPrimaryDock(tree);
    if (rootPrimary) {
        m_mainWindow->addDockWidget(Qt::LeftDockWidgetArea, rootPrimary);
        rootPrimary->show();
        buildSubtreeTopDown(tree, rootPrimary);
    }

    // 5. Always anchor StudioFooterBar at BottomDockWidgetArea
    auto *footerDock = resolveDock(QStringLiteral("StudioFooterBar"));
    if (footerDock) {
        m_mainWindow->addDockWidget(Qt::BottomDockWidgetArea, footerDock);
        footerDock->show();
        activeDockIds.insert(footerDock->panelId());
    }

    // 6. Handle floating panels if declared
    if (tree.contains(QStringLiteral("floating"))) {
        QJsonArray floatingArr = tree.value(QStringLiteral("floating")).toArray();
        for (const auto &itemVal : floatingArr) {
            QJsonObject fObj = itemVal.toObject();
            QString id = fObj.value(QStringLiteral("id")).toString();
            auto *dock = resolveDock(id);
            if (dock) {
                m_mainWindow->addDockWidget(Qt::LeftDockWidgetArea, dock);
                dock->setFloating(true);
                int x = fObj.value(QStringLiteral("x")).toInt(100);
                int y = fObj.value(QStringLiteral("y")).toInt(100);
                int w = fObj.value(QStringLiteral("w")).toInt(600);
                int h = fObj.value(QStringLiteral("h")).toInt(400);
                dock->setGeometry(x, y, w, h);
                dock->show();
                activeDockIds.insert(dock->panelId());
            }
        }
    }

    // 7. Apply proportional split sizes now and on next event loop tick
    applyCurrentSplitSizes();
    QTimer::singleShot(0, this, [this]() {
        m_applyingLayout = true;
        applyCurrentSplitSizes();
        m_applyingLayout = false;
    });

    m_applyingLayout = false;

    for (auto *dock : m_docks) {
        emit panelVisibilityChanged(dock->panelId(), activeDockIds.contains(dock->panelId()));
    }
}

QJsonObject DockLayoutManager::serializePerspectiveTree(const QString &perspectiveName) const
{
    if (m_perspectives.contains(perspectiveName)) {
        return m_perspectives.value(perspectiveName);
    }
    return createDefaultPerspectiveTree();
}

bool DockLayoutManager::deserializePerspectiveTree(const QString &perspectiveName,
                                                   const QJsonObject &perspectiveObj)
{
    if (perspectiveObj.isEmpty())
        return false;
    m_perspectives.insert(perspectiveName, perspectiveObj);
    return true;
}

QJsonObject DockLayoutManager::serializeLayout() const
{
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("layout_revision"), 4);
    root.insert(QStringLiteral("active_perspective"), m_activePerspective);

    QJsonObject perspsObj;
    for (auto it = m_perspectives.constBegin(); it != m_perspectives.constEnd(); ++it) {
        perspsObj.insert(it.key(), it.value());
    }
    root.insert(QStringLiteral("perspectives"), perspsObj);

    return root;
}

bool DockLayoutManager::deserializeLayout(const QJsonObject &root)
{
    if (root.isEmpty() || root.value(QStringLiteral("version")).toInt() != 1) {
        return false;
    }

    const int revision = root.value(QStringLiteral("layout_revision")).toInt(0);
    QJsonObject perspsObj = root.value(QStringLiteral("perspectives")).toObject();

    if (revision < 4) {
        // Refresh all 4 built-in perspectives to current canonical layout trees
        setupDefaultPerspectives();
        for (auto it = perspsObj.constBegin(); it != perspsObj.constEnd(); ++it) {
            if (!m_perspectives.contains(it.key())) {
                m_perspectives.insert(it.key(), it.value().toObject());
            }
        }
    } else {
        for (auto it = perspsObj.constBegin(); it != perspsObj.constEnd(); ++it) {
            m_perspectives.insert(it.key(), it.value().toObject());
        }
    }

    QString active =
        root.value(QStringLiteral("active_perspective")).toString(QStringLiteral("Default"));
    switchPerspective(active);
    return true;
}

QString DockLayoutManager::defaultLayoutFilePath() const
{
    QString configDir = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (configDir.isEmpty()) {
        configDir = QStandardPaths::writableLocation(QStandardPaths::HomeLocation) +
                    QStringLiteral("/.config");
    }
    return configDir + QStringLiteral("/wwal/studio_layout.json");
}

bool DockLayoutManager::saveLayoutToFile(const QString &filePath)
{
    QString targetPath = filePath.isEmpty() ? defaultLayoutFilePath() : filePath;

    QFileInfo fileInfo(targetPath);
    QDir dir = fileInfo.dir();
    if (!dir.exists()) {
        dir.mkpath(QStringLiteral("."));
    }

    QJsonObject root = serializeLayout();
    QJsonDocument doc(root);

    QFile file(targetPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        qWarning() << "Failed to open layout file for writing:" << targetPath;
        return false;
    }

    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();
    return true;
}

bool DockLayoutManager::loadLayoutFromFile(const QString &filePath)
{
    QString targetPath = filePath.isEmpty() ? defaultLayoutFilePath() : filePath;

    QFile file(targetPath);
    if (!file.exists()) {
        // Apply active default perspective first and save initial defaults if missing
        switchPerspective(m_activePerspective);
        saveLayoutToFile(targetPath);
        return true;
    }

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "Failed to open layout file for reading:" << targetPath;
        switchPerspective(m_activePerspective);
        return false;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonParseError parseError{};
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "JSON parse error reading layout file:" << parseError.errorString();
        switchPerspective(m_activePerspective);
        return false;
    }

    QJsonObject rootObj = doc.object();
    const bool needsMigrationSave = (rootObj.value(QStringLiteral("layout_revision")).toInt(0) < 4);
    if (!deserializeLayout(rootObj)) {
        switchPerspective(m_activePerspective);
        return false;
    }
    if (needsMigrationSave) {
        saveLayoutToFile(targetPath);
    }
    return true;
}

/* ========================================================================= */
/* Built-In Perspective Tree Definitions                                     */
/* ========================================================================= */

QJsonObject DockLayoutManager::createDefaultPerspectiveTree()
{
    QJsonObject tree;
    tree.insert(QStringLiteral("split_direction"), QStringLiteral("Horizontal"));
    tree.insert(QStringLiteral("split_ratio"), 0.23);

    // Left: Wallpapers & Workshop browser
    QJsonObject leftTab;
    leftTab.insert(QStringLiteral("type"), QStringLiteral("TabGroup"));
    leftTab.insert(QStringLiteral("active_tab"), QStringLiteral("Wallpapers"));
    QJsonArray leftTabs;
    leftTabs.append(QStringLiteral("Wallpapers"));
    leftTabs.append(QStringLiteral("WorkshopBrowser"));
    leftTabs.append(QStringLiteral("OpenHubBrowser"));
    leftTab.insert(QStringLiteral("tabs"), leftTabs);
    tree.insert(QStringLiteral("left"), leftTab);

    // Right: Center (Viewport + Bezier/Audio) + Right (Inspector/Theme)
    QJsonObject centerVertical;
    centerVertical.insert(QStringLiteral("split_direction"), QStringLiteral("Vertical"));
    centerVertical.insert(QStringLiteral("split_ratio"), 0.59);

    QJsonObject centerTop;
    centerTop.insert(QStringLiteral("type"), QStringLiteral("DockNode"));
    centerTop.insert(QStringLiteral("content"), QStringLiteral("GpuPreviewViewport"));
    centerVertical.insert(QStringLiteral("top"), centerTop);

    QJsonObject centerBottom;
    centerBottom.insert(QStringLiteral("type"), QStringLiteral("TabGroup"));
    centerBottom.insert(QStringLiteral("active_tab"), QStringLiteral("CubicBezier"));
    QJsonArray bottomTabs;
    bottomTabs.append(QStringLiteral("CubicBezier"));
    bottomTabs.append(QStringLiteral("AudioFFT"));
    bottomTabs.append(QStringLiteral("Telemetry"));
    centerBottom.insert(QStringLiteral("tabs"), bottomTabs);
    centerVertical.insert(QStringLiteral("bottom"), centerBottom);

    QJsonObject rightSplit;
    rightSplit.insert(QStringLiteral("split_direction"), QStringLiteral("Horizontal"));
    rightSplit.insert(QStringLiteral("split_ratio"), 0.62);
    rightSplit.insert(QStringLiteral("left"), centerVertical);

    QJsonObject inspectorTab;
    inspectorTab.insert(QStringLiteral("type"), QStringLiteral("TabGroup"));
    inspectorTab.insert(QStringLiteral("active_tab"), QStringLiteral("PropertyInspectorWidget"));
    QJsonArray inspectorTabs;
    inspectorTabs.append(QStringLiteral("PropertyInspectorWidget"));
    inspectorTabs.append(QStringLiteral("ThemeStudioWidget"));
    inspectorTab.insert(QStringLiteral("tabs"), inspectorTabs);
    rightSplit.insert(QStringLiteral("right"), inspectorTab);

    tree.insert(QStringLiteral("right"), rightSplit);

    return tree;
}

QJsonObject DockLayoutManager::createShaderCodingPerspectiveTree()
{
    QJsonObject tree;
    tree.insert(QStringLiteral("split_direction"), QStringLiteral("Horizontal"));
    tree.insert(QStringLiteral("split_ratio"), 0.56);

    // Left: Expanded GPU Preview Viewport
    QJsonObject leftNode;
    leftNode.insert(QStringLiteral("type"), QStringLiteral("DockNode"));
    leftNode.insert(QStringLiteral("content"), QStringLiteral("GpuPreviewViewport"));
    tree.insert(QStringLiteral("left"), leftNode);

    // Right: Inspector on top, Transition & Easing on bottom
    QJsonObject rightSplit;
    rightSplit.insert(QStringLiteral("split_direction"), QStringLiteral("Vertical"));
    rightSplit.insert(QStringLiteral("split_ratio"), 0.62);

    QJsonObject inspectorTab;
    inspectorTab.insert(QStringLiteral("type"), QStringLiteral("TabGroup"));
    inspectorTab.insert(QStringLiteral("active_tab"), QStringLiteral("PropertyInspectorWidget"));
    QJsonArray inspectorTabs;
    inspectorTabs.append(QStringLiteral("PropertyInspectorWidget"));
    inspectorTabs.append(QStringLiteral("ThemeStudioWidget"));
    inspectorTabs.append(QStringLiteral("Wallpapers"));
    inspectorTab.insert(QStringLiteral("tabs"), inspectorTabs);
    rightSplit.insert(QStringLiteral("top"), inspectorTab);

    QJsonObject telemetryTab;
    telemetryTab.insert(QStringLiteral("type"), QStringLiteral("TabGroup"));
    telemetryTab.insert(QStringLiteral("active_tab"), QStringLiteral("CubicBezier"));
    QJsonArray telemetryTabs;
    telemetryTabs.append(QStringLiteral("CubicBezier"));
    telemetryTabs.append(QStringLiteral("Telemetry"));
    telemetryTabs.append(QStringLiteral("AudioFFT"));
    telemetryTab.insert(QStringLiteral("tabs"), telemetryTabs);
    rightSplit.insert(QStringLiteral("bottom"), telemetryTab);

    tree.insert(QStringLiteral("right"), rightSplit);

    return tree;
}

QJsonObject DockLayoutManager::createThemeStudioPerspectiveTree()
{
    QJsonObject tree;
    tree.insert(QStringLiteral("split_direction"), QStringLiteral("Horizontal"));
    tree.insert(QStringLiteral("split_ratio"), 0.25);

    // Left: Wallpaper Library
    QJsonObject leftTab;
    leftTab.insert(QStringLiteral("type"), QStringLiteral("TabGroup"));
    leftTab.insert(QStringLiteral("active_tab"), QStringLiteral("Wallpapers"));
    QJsonArray leftTabs;
    leftTabs.append(QStringLiteral("Wallpapers"));
    leftTab.insert(QStringLiteral("tabs"), leftTabs);
    tree.insert(QStringLiteral("left"), leftTab);

    // Right: Center ThemeStudio + Viewport & Inspector
    QJsonObject rightSplit;
    rightSplit.insert(QStringLiteral("split_direction"), QStringLiteral("Horizontal"));
    rightSplit.insert(QStringLiteral("split_ratio"), 0.55);

    QJsonObject centerTheme;
    centerTheme.insert(QStringLiteral("type"), QStringLiteral("DockNode"));
    centerTheme.insert(QStringLiteral("content"), QStringLiteral("ThemeStudioWidget"));
    rightSplit.insert(QStringLiteral("left"), centerTheme);

    QJsonObject rightSubSplit;
    rightSubSplit.insert(QStringLiteral("split_direction"), QStringLiteral("Vertical"));
    rightSubSplit.insert(QStringLiteral("split_ratio"), 0.54);

    QJsonObject topInspector;
    topInspector.insert(QStringLiteral("type"), QStringLiteral("DockNode"));
    topInspector.insert(QStringLiteral("content"), QStringLiteral("PropertyInspectorWidget"));
    rightSubSplit.insert(QStringLiteral("top"), topInspector);

    QJsonObject bottomViewport;
    bottomViewport.insert(QStringLiteral("type"), QStringLiteral("DockNode"));
    bottomViewport.insert(QStringLiteral("content"), QStringLiteral("GpuPreviewViewport"));
    rightSubSplit.insert(QStringLiteral("bottom"), bottomViewport);

    rightSplit.insert(QStringLiteral("right"), rightSubSplit);
    tree.insert(QStringLiteral("right"), rightSplit);

    return tree;
}

QJsonObject DockLayoutManager::createMultiMonitorPerspectiveTree()
{
    QJsonObject tree;
    tree.insert(QStringLiteral("split_direction"), QStringLiteral("Vertical"));
    tree.insert(QStringLiteral("split_ratio"), 0.32);

    // Top: Monitor Topology Canvas & Bar
    QJsonObject topNode;
    topNode.insert(QStringLiteral("type"), QStringLiteral("DockNode"));
    topNode.insert(QStringLiteral("content"), QStringLiteral("MonitorTopologyBar"));
    tree.insert(QStringLiteral("top"), topNode);

    // Bottom: 3-column layout (Wallpapers / Viewport + Bezier / Inspector)
    QJsonObject bottomSplit;
    bottomSplit.insert(QStringLiteral("split_direction"), QStringLiteral("Horizontal"));
    bottomSplit.insert(QStringLiteral("split_ratio"), 0.25);

    QJsonObject leftTab;
    leftTab.insert(QStringLiteral("type"), QStringLiteral("TabGroup"));
    leftTab.insert(QStringLiteral("active_tab"), QStringLiteral("Wallpapers"));
    QJsonArray leftTabs;
    leftTabs.append(QStringLiteral("Wallpapers"));
    leftTab.insert(QStringLiteral("tabs"), leftTabs);
    bottomSplit.insert(QStringLiteral("left"), leftTab);

    QJsonObject centerRightSplit;
    centerRightSplit.insert(QStringLiteral("split_direction"), QStringLiteral("Horizontal"));
    centerRightSplit.insert(QStringLiteral("split_ratio"), 0.65);

    QJsonObject centerVertical;
    centerVertical.insert(QStringLiteral("split_direction"), QStringLiteral("Vertical"));
    centerVertical.insert(QStringLiteral("split_ratio"), 0.60);

    QJsonObject viewportNode;
    viewportNode.insert(QStringLiteral("type"), QStringLiteral("DockNode"));
    viewportNode.insert(QStringLiteral("content"), QStringLiteral("GpuPreviewViewport"));
    centerVertical.insert(QStringLiteral("top"), viewportNode);

    QJsonObject bezierNode;
    bezierNode.insert(QStringLiteral("type"), QStringLiteral("DockNode"));
    bezierNode.insert(QStringLiteral("content"), QStringLiteral("CubicBezier"));
    centerVertical.insert(QStringLiteral("bottom"), bezierNode);

    centerRightSplit.insert(QStringLiteral("left"), centerVertical);

    QJsonObject inspectorTab;
    inspectorTab.insert(QStringLiteral("type"), QStringLiteral("TabGroup"));
    inspectorTab.insert(QStringLiteral("active_tab"), QStringLiteral("PropertyInspectorWidget"));
    QJsonArray inspectorTabs;
    inspectorTabs.append(QStringLiteral("PropertyInspectorWidget"));
    inspectorTabs.append(QStringLiteral("ThemeStudioWidget"));
    inspectorTab.insert(QStringLiteral("tabs"), inspectorTabs);
    centerRightSplit.insert(QStringLiteral("right"), inspectorTab);

    bottomSplit.insert(QStringLiteral("right"), centerRightSplit);
    tree.insert(QStringLiteral("bottom"), bottomSplit);

    return tree;
}

} // namespace waywal::studio
