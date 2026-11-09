// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QWidget>
#include <memory>
#include <cstdint>

class QLineEdit;
class QComboBox;
class QToolButton;

namespace wwal::studio {

class VirtualizedGridWidget;
class CatalogIndexManager;
class DownloadManager;

class WorkshopBrowserWidget : public QWidget {
    Q_OBJECT

  public:
    explicit WorkshopBrowserWidget(std::shared_ptr<CatalogIndexManager> indexMgr,
                                   std::shared_ptr<DownloadManager> downloadMgr,
                                   QWidget *parent = nullptr);
    ~WorkshopBrowserWidget() override;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  signals:
    void wallpaperSelected(uint64_t publishedFileId, const QString &filePath);
    void wallpaperApplyRequested(uint64_t publishedFileId, const QString &targetOutput);

  public slots:
    void refreshLibrary();
    void setFilterQuery(const QString &query);
    void setCategoryFilter(int categoryIndex);
    void setSortOrder(int sortOrderIndex);

  private slots:
    void onItemDoubleClicked(int itemIndex);
    void onItemHoverStarted(int itemIndex);
    void onDownloadProgressUpdated(uint64_t publishedFileId, float progressPct, float speedBps);

  private:
    void setupUi();
    void wireSignals();

    std::shared_ptr<CatalogIndexManager> m_indexMgr;
    std::shared_ptr<DownloadManager> m_downloadMgr;

    QLineEdit *m_searchEdit{nullptr};
    QComboBox *m_sortCombo{nullptr};
    QComboBox *m_typeCombo{nullptr};
    QToolButton *m_refreshBtn{nullptr};
    VirtualizedGridWidget *m_gridView{nullptr};

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace wwal::studio
