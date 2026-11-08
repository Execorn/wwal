// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WWAL_STUDIO_GRID_VIRTUALIZED_GRID_WIDGET_HPP
#define WWAL_STUDIO_GRID_VIRTUALIZED_GRID_WIDGET_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_set>
#include <memory>
#include <optional>

#include <QWidget>
#include <QOpenGLWidget>
#include <QOpenGLExtraFunctions>
#include <QScrollBar>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPainter>

#include "fuzzy_catalog_index.hpp"
#include "gpu_texture_pool.hpp"
#include "hover_preview_controller.hpp"

namespace wwal::studio {

struct GridVisibleSlice {
    bool is_empty{true};
    int row_first{0};
    int row_last{0};
    size_t index_first{0};
    size_t index_last{0};
};

class GridGlCanvas : public QOpenGLWidget, protected QOpenGLExtraFunctions {
    Q_OBJECT

public:
    static constexpr int ITEM_WIDTH = 280;
    static constexpr int ITEM_HEIGHT = 158; // 16:9 aspect ratio
    static constexpr int SPACING_X = 16;
    static constexpr int SPACING_Y = 16;
    static constexpr int ROW_HEIGHT = ITEM_HEIGHT + SPACING_Y; // 174 px
    static constexpr int OVERSCAN_ROWS = 2; // 2-row overscan margin

    explicit GridGlCanvas(FuzzyCatalogIndex &index, GpuTexturePool &pool,
                          HoverPreviewController &hover, QWidget *parent = nullptr);
    ~GridGlCanvas() override;

    // Uniform Spatial Indexing (O(1))
    [[nodiscard]] int computeColumns(int container_width) const noexcept;
    [[nodiscard]] int computeTotalRows(size_t total_items, int columns) const noexcept;
    [[nodiscard]] int computeVirtualHeight(size_t total_items, int columns) const noexcept;
    [[nodiscard]] GridVisibleSlice computeVisibleSlice(size_t total_items, int columns,
                                                       int scroll_y, int viewport_h) const noexcept;

    void setScrollY(int scroll_y);
    [[nodiscard]] int scrollY() const noexcept { return m_scrollY; }

    [[nodiscard]] std::optional<size_t> itemAtPoint(const QPoint &pos) const;
    [[nodiscard]] QRect itemRect(size_t filtered_idx, int columns, int scroll_y) const;

    void toggleSelection(uint32_t item_id);
    void selectRange(uint32_t from_id, uint32_t to_id);
    void clearSelection();
    void resetHoverState();
    [[nodiscard]] const std::unordered_set<uint32_t> &selectedIds() const noexcept {
        return m_selectedIds;
    }

signals:
    void virtualHeightChanged(int virtual_height);
    void itemSelected(const CatalogItem &item);
    void itemDoubleClicked(const CatalogItem &item);
    void selectionChanged(const std::vector<uint32_t> &selected_ids);
    void hoverChanged(std::optional<size_t> filtered_index);
    void excludeFolderRequested(const QString &folder_path);
    void configureFoldersRequested();

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    FuzzyCatalogIndex &m_index;
    GpuTexturePool &m_pool;
    HoverPreviewController &m_hover;

    int m_scrollY{0};
    std::optional<size_t> m_hoveredFilteredIdx;
    std::unordered_set<uint32_t> m_selectedIds;
    std::optional<uint32_t> m_lastSelectedId;

    // Instanced Quad / Shader state
    GLuint m_cardProgram{0};
    GLuint m_cardVao{0};
    GLuint m_cardVbo{0};
    bool m_glInitialized{false};
    QLabel *m_fallbackCanvas{nullptr};

    void initCardShaders();
    void renderCards(QPainter &painter);
    void drawBadgePill(QPainter &painter, const QRect &card_rect, WallpaperBadge badge);
    void drawCardSelectionIndicator(QPainter &painter, const QRect &card_rect, bool is_selected);
    void drawCardTitleOverlay(QPainter &painter, const QRect &card_rect, const std::string &title);
};

class VirtualizedGridWidget : public QWidget {
    Q_OBJECT

public:
    explicit VirtualizedGridWidget(QWidget *parent = nullptr);
    ~VirtualizedGridWidget() override;

    // Discovers wallpapers and populates catalog
    void scanWallpapers(const std::vector<std::string> &extra_paths = {});
    void populateSampleCatalogIfEmpty(bool force_showcase_cards = false);
    void selectFirstItemIfAvailable();

public slots:
    void openFolderSettingsDialog();

public:
    // Accessors
    [[nodiscard]] const FuzzyCatalogIndex &catalogIndex() const noexcept { return m_index; }
    [[nodiscard]] FuzzyCatalogIndex &catalogIndex() noexcept { return m_index; }
    [[nodiscard]] GpuTexturePool &texturePool() noexcept { return m_pool; }
    [[nodiscard]] HoverPreviewController &hoverController() noexcept { return m_hover; }

    [[nodiscard]] std::vector<uint32_t> selectedIds() const;
    void clearSelection();

signals:
    void wallpaperSelected(const CatalogItem &item);
    void wallpaperDoubleClicked(const CatalogItem &item);
    void playlistSelectionChanged(const std::vector<uint32_t> &selected_ids);
    void catalogScanned(size_t total_count);

private slots:
    void onSearchTextChanged(const QString &text);
    void onTagChipClicked(uint64_t tag_mask);
    void onSortChanged(int combo_index);
    void onScrollValueChanged(int value);
    void onVirtualHeightChanged(int virtual_height);
    void onPlaylistAddClicked();

private:
    FuzzyCatalogIndex m_index;
    GpuTexturePool m_pool;
    HoverPreviewController m_hover;

    // UI Widgets
    QLineEdit *m_searchEdit{nullptr};
    QComboBox *m_sortCombo{nullptr};
    QPushButton *m_foldersButton{nullptr};
    QLabel *m_statusLabel{nullptr};
    QWidget *m_tagChipContainer{nullptr};
    std::vector<QPushButton *> m_tagChips;

    GridGlCanvas *m_canvas{nullptr};
    QScrollBar *m_scrollBar{nullptr};

    // Playlist Bar
    QWidget *m_playlistBar{nullptr};
    QLabel *m_playlistSelectionLabel{nullptr};
    QPushButton *m_playlistAddButton{nullptr};
    QPushButton *m_playlistClearButton{nullptr};

    void setupUi();
    void updateTagChipStyles();
    void updateStatusText();
};

} // namespace wwal::studio

#endif // WWAL_STUDIO_GRID_VIRTUALIZED_GRID_WIDGET_HPP
