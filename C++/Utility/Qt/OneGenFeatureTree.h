#pragma once

#include <QHash>
#include <QColor>
#include <QSet>
#include <QTreeWidget>

class QFrame;
class QGraphicsDropShadowEffect;
class QLabel;
class QLineEdit;
class QToolButton;
class QTimer;

/** Feature browser with local filtering and shared node-map favorites. */
class OneGenFeatureTree final : public QTreeWidget {
  Q_OBJECT
  Q_PROPERTY(int searchInset READ searchInset WRITE setSearchInset)
  Q_PROPERTY(int searchMaximumWidth READ searchMaximumWidth WRITE setSearchMaximumWidth)
  Q_PROPERTY(QColor favoriteColor READ favoriteColor WRITE setFavoriteColor)
  Q_PROPERTY(qreal searchShadowBlur READ searchShadowBlur WRITE setSearchShadowBlur)
  Q_PROPERTY(int searchShadowOffset READ searchShadowOffset WRITE setSearchShadowOffset)
  Q_PROPERTY(QColor searchShadowColor READ searchShadowColor WRITE setSearchShadowColor)
public:
  enum Role { NodeNameRole = Qt::UserRole, MapRole, LeafRole, ModuleNameRole, FavoriteRole };
  explicit OneGenFeatureTree(QWidget* parent = nullptr);
  /** Marks an existing editable row without replacing its editor or hierarchy. */
  void registerFeature(QTreeWidgetItem* item, const QString& moduleName);
  /** Toggles and persists a leaf's favorite state without contacting the producer. */
  void toggleFavorite(QTreeWidgetItem* item);
  /** Reports whether this node-map/name pair is saved in the shared favorite list. */
  bool isFavorite(const QTreeWidgetItem* item) const;
  /** Reapplies local filters after a full or incremental feature snapshot. */
  void refreshFilter();
  /** Returns the theme-controlled overlay inset. */
  int searchInset() const { return search_inset_; }
  /** Applies the theme-controlled overlay inset. */
  void setSearchInset(int value);
  /** Returns the theme-controlled maximum bubble width. */
  int searchMaximumWidth() const { return search_maximum_width_; }
  /** Applies the theme-controlled maximum bubble width. */
  void setSearchMaximumWidth(int value);
  /** Returns the theme color for saved favorites. */
  QColor favoriteColor() const { return favorite_color_; }
  /** Applies the theme color for saved favorites. */
  void setFavoriteColor(const QColor& color);
  /** Returns the theme-controlled search surface shadow blur. */
  qreal searchShadowBlur() const { return search_shadow_blur_; }
  /** Applies the theme-controlled search surface shadow blur. */
  void setSearchShadowBlur(qreal value);
  /** Returns the theme-controlled vertical shadow offset. */
  int searchShadowOffset() const { return search_shadow_offset_; }
  /** Applies the theme-controlled vertical shadow offset. */
  void setSearchShadowOffset(int value);
  /** Returns the theme-controlled search surface shadow color. */
  QColor searchShadowColor() const { return search_shadow_color_; }
  /** Applies the theme-controlled search surface shadow color. */
  void setSearchShadowColor(const QColor& color);

protected:
  bool viewportEvent(QEvent* event) override;
  void scrollContentsBy(int dx, int dy) override;
  void resizeEvent(QResizeEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void updateGeometries() override;

private:
  QString featureKey(const QTreeWidgetItem* item) const;
  QString expansionKey(const QTreeWidgetItem* item) const;
  QString settingsPath() const;
  void loadFavorites();
  void filterChanged();
  bool filterBranch(QTreeWidgetItem* item, const QString& ancestors);
  void positionSearch();
  void updateSearchShadow();

  QFrame* search_bubble_ = nullptr;
  QGraphicsDropShadowEffect* search_shadow_ = nullptr;
  QLineEdit* search_ = nullptr;
  QToolButton* favorites_only_ = nullptr;
  QLabel* result_count_ = nullptr;
  QTimer* filter_timer_ = nullptr;
  QSet<QString> favorites_;
  QStringList filter_tokens_;
  QHash<QString, bool> saved_expansion_;
  QString saved_current_;
  int saved_vertical_scroll_ = 0;
  int saved_horizontal_scroll_ = 0;
  int matched_ = 0;
  int total_ = 0;
  int search_inset_ = 0;
  int search_maximum_width_ = 0;
  QColor favorite_color_;
  qreal search_shadow_blur_ = 0;
  int search_shadow_offset_ = 0;
  QColor search_shadow_color_ = Qt::transparent;
  bool filtering_ = false;
  bool refreshing_filter_ = false;
  bool updating_geometry_ = false;
};
