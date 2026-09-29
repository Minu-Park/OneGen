#include "OneGenFeatureTree.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QIconEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QStandardPaths>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidgetItemIterator>
#include <algorithm>
#include <cmath>

namespace {

void paintStar(QPainter* painter, const QRectF& rect, bool filled, const QColor& color) {
  painter->save();
  painter->setRenderHint(QPainter::Antialiasing);
  QPainterPath star;
  for (int point = 0; point < 10; ++point) {
    const double angle = (point * 36.0 - 90.0) * 3.141592653589793 / 180.0;
    const double radius = rect.width() * (point % 2 ? 0.22 : 0.5);
    const QPointF vertex(rect.center().x() + radius * std::cos(angle),
                         rect.center().y() + radius * std::sin(angle));
    if (point == 0) star.moveTo(vertex); else star.lineTo(vertex);
  }
  star.closeSubpath();
  painter->setPen(color);
  painter->setBrush(filled ? QBrush(color) : Qt::NoBrush);
  painter->drawPath(star);
  painter->restore();
}

/** Scalable star icon with no dependency on installed symbol fonts or Resources. */
class StarIconEngine final : public QIconEngine {
public:
  explicit StarIconEngine(OneGenFeatureTree* tree) : tree_(tree) {}
  QIconEngine* clone() const override { return new StarIconEngine(tree_); }
  QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
    QPixmap result(size);
    result.fill(Qt::transparent);
    QPainter painter(&result);
    paint(&painter, QRect(QPoint(), size), mode, state);
    return result;
  }
  void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override {
    const bool filled = state == QIcon::On;
    auto color = filled ? tree_->favoriteColor() : tree_->palette().color(QPalette::Text);
    if (mode == QIcon::Disabled) color = tree_->palette().color(QPalette::Disabled, QPalette::Text);
    paintStar(painter, rect.adjusted(2, 2, -2, -2), filled, color);
  }
private:
  OneGenFeatureTree* tree_;
};

QString tupleKey(const QStringList& parts) {
  return QString::fromLatin1(QJsonDocument(QJsonArray::fromStringList(parts))
      .toJson(QJsonDocument::Compact).toBase64(QByteArray::Base64UrlEncoding));
}

/** Paints and handles stars in the name cell, leaving the Value editor untouched. */
class FavoriteDelegate final : public QStyledItemDelegate {
public:
  explicit FavoriteDelegate(OneGenFeatureTree* tree) : QStyledItemDelegate(tree), tree_(tree) {}

  QRect starRect(const QStyleOptionViewItem& option) const {
    const int extent = tree_->style()->pixelMetric(QStyle::PM_SmallIconSize);
    return QRect(option.rect.right() - extent - 3, option.rect.center().y() - extent / 2,
                 extent, extent);
  }

  void paint(QPainter* painter, const QStyleOptionViewItem& option,
             const QModelIndex& index) const override {
    auto label = option;
    const bool leaf = index.data(OneGenFeatureTree::LeafRole).toBool();
    if (leaf) label.rect.setRight(starRect(option).left() - 3);
    QStyledItemDelegate::paint(painter, label, index);
    if (!leaf) return;
    const bool favorite = index.data(OneGenFeatureTree::FavoriteRole).toBool();
    painter->save();
    const auto rect = starRect(option).adjusted(2, 2, -2, -2);
    const auto color = favorite ? tree_->favoriteColor() : option.palette.color(QPalette::Text);
    painter->setOpacity(favorite || option.state.testFlag(QStyle::State_MouseOver) ? 1.0 : 0.3);
    paintStar(painter, rect, favorite, color);
    painter->restore();
  }

  bool editorEvent(QEvent* event, QAbstractItemModel*, const QStyleOptionViewItem& option,
                   const QModelIndex& index) override {
    if (!index.data(OneGenFeatureTree::LeafRole).toBool()) return false;
    if (event->type() != QEvent::MouseButtonPress && event->type() != QEvent::MouseButtonRelease)
      return false;
    const auto* mouse = static_cast<QMouseEvent*>(event);
    if (mouse->button() != Qt::LeftButton || !starRect(option).contains(mouse->position().toPoint()))
      return false;
    if (event->type() == QEvent::MouseButtonRelease) {
      const auto* item = tree_->itemAt(option.rect.center());
      tree_->toggleFavorite(const_cast<QTreeWidgetItem*>(item));
    }
    return true;
  }

private:
  OneGenFeatureTree* tree_;
};

} // namespace

OneGenFeatureTree::OneGenFeatureTree(QWidget* parent) : QTreeWidget(parent) {
  setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  setItemDelegateForColumn(0, new FavoriteDelegate(this));
  search_inset_ = style()->pixelMetric(QStyle::PM_LayoutLeftMargin);
  favorite_color_ = QApplication::palette().color(QPalette::Highlight);

  search_bubble_ = new QFrame(viewport());
  search_bubble_->setObjectName(QStringLiteral("DeviceFeatureSearchBubble"));
  auto* layout = new QHBoxLayout(search_bubble_);
  layout->setObjectName(QStringLiteral("DeviceFeatureSearchLayout"));

  search_ = new QLineEdit(search_bubble_);
  search_->setObjectName(QStringLiteral("DeviceFeatureSearchInput"));
  search_->setPlaceholderText(tr("Search features"));
  search_->setAccessibleName(tr("Search features"));
  search_->setToolTip(tr("Search feature names and categories (Ctrl+F). Escape clears the search."));
  search_->setMinimumWidth(0);
  auto* clearSearch = new QToolButton(search_bubble_);
  clearSearch->setObjectName(QStringLiteral("DeviceFeatureSearchClear"));
  clearSearch->setIcon(style()->standardIcon(QStyle::SP_DialogCloseButton));
  clearSearch->setToolButtonStyle(Qt::ToolButtonIconOnly);
  clearSearch->setFocusPolicy(Qt::NoFocus);
  clearSearch->setToolTip(tr("Clear search"));
  clearSearch->setAccessibleName(tr("Clear search"));
  result_count_ = new QLabel(search_bubble_);
  result_count_->setObjectName(QStringLiteral("DeviceFeatureSearchCount"));
  result_count_->setAccessibleName(tr("Matching features"));
  favorites_only_ = new QToolButton(search_bubble_);
  favorites_only_->setObjectName(QStringLiteral("DeviceFeatureFavoritesOnly"));
  favorites_only_->setIcon(QIcon(new StarIconEngine(this)));
  favorites_only_->setCheckable(true);
  favorites_only_->setToolTip(tr("Show favorites only"));
  favorites_only_->setAccessibleName(tr("Show favorites only"));
  favorites_only_->setIconSize(QSize(16, 16));
  layout->addWidget(search_, 1);
  layout->addWidget(clearSearch);
  layout->addWidget(result_count_);
  layout->addWidget(favorites_only_);
  result_count_->hide();
  clearSearch->hide();
  connect(clearSearch, &QToolButton::clicked, search_, [this] {
    search_->clear();
    search_->setFocus(Qt::MouseFocusReason);
  });
  connect(search_, &QLineEdit::textChanged, clearSearch, [clearSearch](const QString& text) {
    clearSearch->setVisible(!text.isEmpty());
  });
  connect(search_, &QLineEdit::textChanged, this, &OneGenFeatureTree::filterChanged);
  connect(favorites_only_, &QToolButton::toggled, this, [this] {
    filterChanged();
  });
  auto* find = new QShortcut(QKeySequence::Find, this);
  find->setContext(Qt::WidgetWithChildrenShortcut);
  connect(find, &QShortcut::activated, search_, [this] { search_->setFocus(); search_->selectAll(); });
  auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), search_);
  escape->setContext(Qt::WidgetShortcut);
  connect(escape, &QShortcut::activated, search_, &QLineEdit::clear);
  auto* favorite = new QShortcut(QKeySequence(QStringLiteral("Ctrl+D")), this);
  favorite->setContext(Qt::WidgetWithChildrenShortcut);
  connect(favorite, &QShortcut::activated, this, [this] { toggleFavorite(currentItem()); });

  filter_timer_ = new QTimer(this);
  filter_timer_->setSingleShot(true);
  connect(filter_timer_, &QTimer::timeout, this, &OneGenFeatureTree::refreshFilter);
  const auto changed = [this] {
    if (!refreshing_filter_) filter_timer_->start();
  };
  connect(model(), &QAbstractItemModel::dataChanged, this, changed);
  connect(model(), &QAbstractItemModel::rowsInserted, this, changed);
  connect(model(), &QAbstractItemModel::rowsRemoved, this, changed);
  connect(model(), &QAbstractItemModel::modelReset, this, changed);
  loadFavorites();
}

QString OneGenFeatureTree::settingsPath() const {
  return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
      .filePath(QStringLiteral("OneGenFeatures.ini"));
}

void OneGenFeatureTree::loadFavorites() {
  QSettings settings(settingsPath(), QSettings::IniFormat);
  const auto saved = settings.value(QStringLiteral("Favorites/Nodes")).toStringList();
  favorites_ = QSet<QString>(saved.begin(), saved.end());
}

QString OneGenFeatureTree::featureKey(const QTreeWidgetItem* item) const {
  return tupleKey({item->data(0, ModuleNameRole).toString(), item->data(0, NodeNameRole).toString()});
}

QString OneGenFeatureTree::expansionKey(const QTreeWidgetItem* item) const {
  return tupleKey({item->data(0, MapRole).toString(), item->data(0, NodeNameRole).toString()});
}

void OneGenFeatureTree::registerFeature(QTreeWidgetItem* item, const QString& moduleName) {
  item->setData(0, LeafRole, true);
  item->setData(0, ModuleNameRole, moduleName);
  item->setData(0, FavoriteRole, isFavorite(item));
}

bool OneGenFeatureTree::isFavorite(const QTreeWidgetItem* item) const {
  return item && item->data(0, LeafRole).toBool() && favorites_.contains(featureKey(item));
}

void OneGenFeatureTree::toggleFavorite(QTreeWidgetItem* item) {
  if (!item || !item->data(0, LeafRole).toBool()) return;
  QSettings settings(settingsPath(), QSettings::IniFormat);
  settings.sync();
  const auto settingsKey = QStringLiteral("Favorites/Nodes");
  const auto saved = settings.value(settingsKey).toStringList();
  favorites_ = QSet<QString>(saved.begin(), saved.end());
  const auto key = featureKey(item);
  if (favorites_.contains(key)) favorites_.remove(key); else favorites_.insert(key);
  QDir().mkpath(QFileInfo(settingsPath()).absolutePath());
  auto sorted = favorites_.values();
  sorted.sort();
  settings.setValue(settingsKey, sorted);
  settings.sync();
  // All open device/Interface panels share the same node-map/name favorite list.
  for (auto* widget : QApplication::allWidgets()) {
    if (auto* tree = qobject_cast<OneGenFeatureTree*>(widget)) {
      tree->loadFavorites();
      tree->refreshFilter();
    }
  }
}

void OneGenFeatureTree::filterChanged() {
  const bool active = !search_->text().trimmed().isEmpty() || favorites_only_->isChecked();
  if (active && !filtering_) {
    saved_expansion_.clear();
    for (QTreeWidgetItemIterator it(this); *it; ++it)
      saved_expansion_.insert(expansionKey(*it), (*it)->isExpanded());
    saved_current_ = currentItem() ? expansionKey(currentItem()) : QString();
    saved_vertical_scroll_ = verticalScrollBar()->value();
    saved_horizontal_scroll_ = horizontalScrollBar()->value();
  }
  filtering_ = active;
  refreshFilter();
  if (!active) {
    for (QTreeWidgetItemIterator it(this); *it; ++it) {
      const auto key = expansionKey(*it);
      if (saved_expansion_.contains(key)) (*it)->setExpanded(saved_expansion_.value(key));
      if (key == saved_current_) setCurrentItem(*it);
    }
    doItemsLayout();
    verticalScrollBar()->setValue(saved_vertical_scroll_);
    horizontalScrollBar()->setValue(saved_horizontal_scroll_);
    saved_expansion_.clear();
  }
}

bool OneGenFeatureTree::filterBranch(QTreeWidgetItem* item, const QString& ancestors) {
  const bool leaf = item->data(0, LeafRole).toBool();
  const auto text = ancestors + QChar(' ') + item->text(0) + QChar(' ')
      + item->data(0, NodeNameRole).toString();
  const bool ownMatch = std::all_of(filter_tokens_.begin(), filter_tokens_.end(),
      [&text](const QString& token) { return text.contains(token, Qt::CaseInsensitive); });
  bool visible = false;
  for (int i = 0; i < item->childCount(); ++i)
    visible = filterBranch(item->child(i), text) || visible;
  if (leaf) {
    ++total_;
    const bool favorite = isFavorite(item);
    if (item->data(0, FavoriteRole).toBool() != favorite) item->setData(0, FavoriteRole, favorite);
    visible = ownMatch
        && (!favorites_only_->isChecked() || favorite);
    if (visible) ++matched_;
  } else if (!filtering_) visible = true;
  item->setHidden(!visible);
  if (filtering_ && visible && !leaf) item->setExpanded(true);
  return visible;
}

void OneGenFeatureTree::refreshFilter() {
  if (refreshing_filter_) return;
  refreshing_filter_ = true;
  filter_tokens_ = search_->text().trimmed().split(QRegularExpression(QStringLiteral("\\s+")),
                                                 Qt::SkipEmptyParts);
  matched_ = total_ = 0;
  for (int i = 0; i < topLevelItemCount(); ++i) filterBranch(topLevelItem(i), {});
  result_count_->setVisible(filtering_);
  result_count_->setText(matched_ ? QStringLiteral("%1/%2").arg(matched_).arg(total_)
                                : tr("No results"));
  result_count_->setAccessibleDescription(tr("%1 of %2 features match").arg(matched_).arg(total_));
  refreshing_filter_ = false;
  doItemsLayout();
  positionSearch();
}

void OneGenFeatureTree::setSearchInset(int value) {
  search_inset_ = qMax(0, value);
  if (search_bubble_) { positionSearch(); updateGeometries(); }
}

void OneGenFeatureTree::setSearchMaximumWidth(int value) {
  search_maximum_width_ = qMax(0, value);
  if (search_bubble_) positionSearch();
}

void OneGenFeatureTree::setFavoriteColor(const QColor& color) {
  favorite_color_ = color;
  if (favorites_only_) favorites_only_->setIcon(QIcon(new StarIconEngine(this)));
  viewport()->update();
}

void OneGenFeatureTree::setSearchShadowBlur(qreal value) {
  search_shadow_blur_ = qMax(qreal(0), value);
  updateSearchShadow();
}

void OneGenFeatureTree::setSearchShadowOffset(int value) {
  search_shadow_offset_ = value;
  updateSearchShadow();
}

void OneGenFeatureTree::setSearchShadowColor(const QColor& color) {
  search_shadow_color_ = color;
  updateSearchShadow();
}

void OneGenFeatureTree::updateSearchShadow() {
  if (!search_bubble_) return;
  const bool enabled = search_shadow_blur_ > 0 && search_shadow_color_.alpha() > 0;
  if (!search_shadow_ && !enabled) return;
  if (!search_shadow_) {
    search_shadow_ = new QGraphicsDropShadowEffect(search_bubble_);
    search_bubble_->setGraphicsEffect(search_shadow_);
  }
  search_shadow_->setBlurRadius(search_shadow_blur_);
  search_shadow_->setOffset(0, search_shadow_offset_);
  search_shadow_->setColor(search_shadow_color_);
  search_shadow_->setEnabled(enabled);
}

void OneGenFeatureTree::positionSearch() {
  if (!search_bubble_) return;
  search_bubble_->ensurePolished();
  const auto area = viewport()->rect();
  const int available = qMax(0, area.width() - 2 * search_inset_);
  const int width = search_maximum_width_ ? qMin(search_maximum_width_, available) : available;
  const int height = search_bubble_->sizeHint().height();
  search_bubble_->setGeometry((area.width() - width) / 2,
      qMax(0, area.height() - height - search_inset_), width, height);
  search_bubble_->raise();
}

void OneGenFeatureTree::updateGeometries() {
  if (updating_geometry_) return;
  updating_geometry_ = true;
  const int previousScroll = verticalScrollBar()->value();
  QTreeWidget::updateGeometries();
  positionSearch();
  if (search_bubble_) {
    // Extend the scrollable content, so the final real row can clear the floating controls.
    int contentHeight = 0;
    for (auto index = model()->index(0, 0); index.isValid(); index = indexBelow(index))
      contentHeight += rowHeight(index);
    const int reserve = search_bubble_->height() + 2 * search_inset_;
    verticalScrollBar()->setRange(0, qMax(0, contentHeight - viewport()->height() + reserve));
    verticalScrollBar()->setPageStep(qMax(1, viewport()->height() - reserve));
    verticalScrollBar()->setValue(previousScroll);
  }
  updating_geometry_ = false;
}

void OneGenFeatureTree::resizeEvent(QResizeEvent* event) {
  QTreeWidget::resizeEvent(event);
  positionSearch();
}

bool OneGenFeatureTree::viewportEvent(QEvent* event) {
  const bool handled = QTreeWidget::viewportEvent(event);
  if (event->type() == QEvent::Resize && search_bubble_) {
    positionSearch();
  }
  return handled;
}

void OneGenFeatureTree::scrollContentsBy(int dx, int dy) {
  QTreeWidget::scrollContentsBy(dx, dy);
  // QAbstractScrollArea moves viewport children along with rows; pin the overlay afterward.
  positionSearch();
}

void OneGenFeatureTree::showEvent(QShowEvent* event) {
  QTreeWidget::showEvent(event);
  positionSearch();
  updateGeometries();
}
