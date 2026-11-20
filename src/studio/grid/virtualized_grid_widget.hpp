// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WWAL_STUDIO_GRID_VIRTUALIZED_GRID_WIDGET_HPP
#define WWAL_STUDIO_GRID_VIRTUALIZED_GRID_WIDGET_HPP

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include <QComboBox>
#include <QDialog>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QOpenGLExtraFunctions>
#include <QOpenGLWidget>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>

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
    static constexpr int ITEM_HEIGHT = 158;
    static constexpr int SPACING_X = 16;
    static constexpr int SPACING_Y = 16;
    static constexpr int ROW_HEIGHT = ITEM_HEIGHT + SPACING_Y;
    static constexpr int OVERSCAN_ROWS = 2;
    static constexpr size_t MAX_COMPRESSED_CARD_TEXTURES = 256;

    explicit GridGlCanvas(FuzzyCatalogIndex &index, GpuTexturePool &pool,
                          HoverPreviewController &hover, QWidget *parent = nullptr);
    ~GridGlCanvas() override;

    // Software ETC2 block encoder (0.5 Bpp, 8 bytes per 4x4 block)
    static void encodeRgbaToEtc2(const uint8_t *rgba, int width, int height,
                                 std::vector<uint8_t> &outBlocks);

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
    void setEmbeddedMode(bool embedded) noexcept { m_embeddedMode = embedded; }
    [[nodiscard]] bool isEmbeddedMode() const noexcept { return m_embeddedMode; }
    [[nodiscard]] const std::vector<uint32_t> &selectedIds() const noexcept
    {
        return m_selectedOrder;
    }

  signals:
    void virtualHeightChanged(int virtual_height);
    void scrollYChanged(int scroll_y);
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
    void keyPressEvent(QKeyEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

  private:
    FuzzyCatalogIndex &m_index;
    GpuTexturePool &m_pool;
    HoverPreviewController &m_hover;

    int m_scrollY{0};
    bool m_embeddedMode{false};
    std::optional<size_t> m_hoveredFilteredIdx;
    std::optional<size_t> m_focusedFilteredIdx;
    std::unordered_set<uint32_t> m_selectedIds;
    std::vector<uint32_t> m_selectedOrder;
    std::optional<uint32_t> m_lastSelectedId;

    // Reusable per-slice rendered flags avoid per-frame unordered_set allocs.
    std::vector<uint8_t> m_glRenderedSliceMask;
    size_t m_glRenderedSliceStart{0};

    GLuint m_cardArrayProgram{0};
    GLuint m_cardVao{0};
    GLuint m_cardVbo{0};
    GLint m_uArrayViewportLoc{-1};
    GLint m_uArrayCardRectLoc{-1};
    GLint m_uArrayTexLoc{-1};
    GLint m_uArrayLayerLoc{-1};
    GLint m_uArraySrgbLoc{-1};
    GLint m_uArrayDprLoc{-1};
    bool m_glInitialized{false};
    QMetaObject::Connection m_contextDestroyConn;

    void ensureIndexVisible(size_t filtered_idx);
    void selectSingleFilteredIndex(size_t filtered_idx);
    void initCardShaders();
    void releaseGlResources();
    void renderGlThumbnails();
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

    void scanWallpapers(const std::vector<std::string> &extra_paths = {});
    void populateSampleCatalogIfEmpty(bool force_showcase_cards = false);
    void selectFirstItemIfAvailable();
    void setEmbeddedMode(bool embedded);
    [[nodiscard]] bool isEmbeddedMode() const noexcept { return m_embeddedMode; }
    void refreshCatalogView();

  public slots:
    void openFolderSettingsDialog();
    void setTagFilter(uint64_t tag_mask);
    void setBadgeFilter(uint32_t badge_mask);

  public:
    [[nodiscard]] const FuzzyCatalogIndex &catalogIndex() const noexcept { return m_index; }
    [[nodiscard]] FuzzyCatalogIndex &catalogIndex() noexcept { return m_index; }
    [[nodiscard]] GpuTexturePool &texturePool() noexcept { return m_pool; }
    [[nodiscard]] HoverPreviewController &hoverController() noexcept { return m_hover; }
    [[nodiscard]] GridGlCanvas *canvas() const noexcept { return m_canvas; }

    [[nodiscard]] std::vector<uint32_t> selectedIds() const;
    [[nodiscard]] const std::vector<uint32_t> &playlistQueue() const noexcept
    {
        return m_playlistQueue;
    }
    void clearSelection();

  signals:
    void wallpaperSelected(const CatalogItem &item);
    void wallpaperDoubleClicked(const CatalogItem &item);
    void playlistSelectionChanged(const std::vector<uint32_t> &selected_ids);
    void playlistAddRequested(const std::vector<uint32_t> &added_ids);
    void catalogScanned(size_t total_count);

  private slots:
    void onSearchTextChanged(const QString &text);
    void onSortChanged(int combo_index);
    void onScrollValueChanged(int value);
    void onVirtualHeightChanged(int virtual_height);
    void onPlaylistAddClicked();

  private:
    FuzzyCatalogIndex m_index;
    GpuTexturePool m_pool;
    HoverPreviewController m_hover;
    bool m_embeddedMode{false};

    QWidget *m_headerContainer{nullptr};
    QLineEdit *m_searchEdit{nullptr};
    QComboBox *m_tagCombo{nullptr};
    QComboBox *m_badgeCombo{nullptr};
    QComboBox *m_sortCombo{nullptr};
    QPushButton *m_foldersButton{nullptr};
    QLabel *m_statusLabel{nullptr};
    QTimer *m_searchDebounceTimer{nullptr};
    QString m_pendingSearchText;
    QPointer<QDialog> m_folderDialog;
    std::vector<uint32_t> m_playlistQueue;

    GridGlCanvas *m_canvas{nullptr};
    QScrollBar *m_scrollBar{nullptr};

    QWidget *m_playlistBar{nullptr};
    QLabel *m_playlistSelectionLabel{nullptr};
    QPushButton *m_playlistAddButton{nullptr};
    QPushButton *m_playlistClearButton{nullptr};

    void setupUi();
    void applySearchText(const QString &text);
    void updateStatusText();
};

} // namespace wwal::studio

#endif // WWAL_STUDIO_GRID_VIRTUALIZED_GRID_WIDGET_HPP
