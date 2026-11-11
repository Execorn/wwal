// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_STUDIO_GRAPH_EFFECT_LIBRARY_BROWSER_WIDGET_HPP
#define WAYWAL_STUDIO_GRAPH_EFFECT_LIBRARY_BROWSER_WIDGET_HPP

#include "waywal/we_builtin_effects.h"

#include <QComboBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <vector>

namespace waywal::studio {

class EffectLibraryBrowserWidget : public QWidget {
    Q_OBJECT

  public:
    explicit EffectLibraryBrowserWidget(QWidget *parent = nullptr);
    ~EffectLibraryBrowserWidget() override = default;

    void setFilterText(const QString &query);
    void setSearchFilter(const QString &text) { setFilterText(text); }
    void setCategoryFilter(int32_t categoryOrNegativeOneForAll);
    [[nodiscard]] we_builtin_effect_id_t selectedEffectId() const noexcept;
    [[nodiscard]] size_t visibleEffectCount() const noexcept;

  signals:
    void effectActivated(we_builtin_effect_id_t effectId, const QString &slug);
    void effectInsertIntoGraphRequested(we_builtin_effect_id_t effectId);
    void effectApplyToActiveSceneRequested(we_builtin_effect_id_t effectId);

    // Aliases matching dispatch requirement
    void effectInsertRequested(uint32_t effectId);
    void effectAppendToStackRequested(uint32_t effectId);

  private slots:
    void onSearchTextChanged(const QString &text);
    void onCategoryComboChanged(int index);
    void onItemDoubleClicked(QListWidgetItem *item);
    void onAddToGraphClicked();
    void onApplyToLayerClicked();

  private:
    void setupUi();
    void populateEffectList();
    void applyFilter();

    QLineEdit *m_searchEdit{nullptr};
    QComboBox *m_categoryCombo{nullptr};
    QListWidget *m_listWidget{nullptr};
    QPushButton *m_btnAddToGraph{nullptr};
    QPushButton *m_btnApplyToScene{nullptr};
    QString m_currentFilter;
    int32_t m_currentCategory{-1};
};

} // namespace waywal::studio

namespace wwal::studio {
using EffectLibraryBrowserWidget = waywal::studio::EffectLibraryBrowserWidget;
}

using waywal::studio::EffectLibraryBrowserWidget;

#endif // WAYWAL_STUDIO_GRAPH_EFFECT_LIBRARY_BROWSER_WIDGET_HPP
