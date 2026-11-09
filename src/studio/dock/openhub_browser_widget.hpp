// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QWidget>
#include <memory>
#include <string>

class QLineEdit;
class QComboBox;
class QToolButton;

namespace wwal::studio {

class VirtualizedGridWidget;
class DownloadManager;

class OpenHubBrowserWidget : public QWidget {
    Q_OBJECT

  public:
    explicit OpenHubBrowserWidget(std::shared_ptr<DownloadManager> downloadMgr,
                                  QWidget *parent = nullptr);
    ~OpenHubBrowserWidget() override;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  signals:
    void packageSelected(const QString &packageId, const QString &cid);
    void packageInstallRequested(const QString &packageId, const QString &targetOutput);

  public slots:
    void refreshFeed();
    void setFilterQuery(const QString &query);
    void setCategoryFilter(int categoryIndex);
    void setSortOrder(int sortOrderIndex);

  private slots:
    void onItemDoubleClicked(int itemIndex);
    void onItemHoverStarted(int itemIndex);
    void onDownloadProgressUpdated(const QString &packageId, float progressPct, float speedBps);

  private:
    void setupUi();
    void wireSignals();

    std::shared_ptr<DownloadManager> m_downloadMgr;

    QLineEdit *m_searchEdit{nullptr};
    QComboBox *m_sortCombo{nullptr};
    QComboBox *m_categoryCombo{nullptr};
    QToolButton *m_refreshBtn{nullptr};
    VirtualizedGridWidget *m_gridView{nullptr};

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace wwal::studio
