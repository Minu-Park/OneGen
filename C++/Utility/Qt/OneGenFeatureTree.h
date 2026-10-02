#pragma once

#include "Widgets/SearchableTreeWidget.h"

/** GenICam node identity and storage adapter for the shared searchable tree. */
class OneGenFeatureTree final : public SearchableTreeWidget {
  Q_OBJECT
public:
  /** Producer-owned metadata roles; common browser roles occupy a separate range. */
  enum Role { NodeNameRole = Qt::UserRole, MapRole };
  /** Creates a feature browser with consumer storage or the standalone OneGen store. */
  explicit OneGenFeatureTree(QWidget* parent = nullptr, const QString& settingsFile = {},
                             const QString& favoritesKey = {});
  /** Registers an existing feature using its shared node-map name and internal name. */
  void registerFeature(QTreeWidgetItem* item, const QString& moduleName);

protected:
  /** Identifies rows within the current snapshot, independently of saved favorite keys. */
  QString itemStateKey(const QTreeWidgetItem* item) const override;
  /** Includes internal node names when searching feature and category rows. */
  QString itemSearchText(const QTreeWidgetItem* item) const override;
};
