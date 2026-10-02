#include "OneGenFeatureTree.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>

namespace {

/** Encodes compound node identities without delimiter collisions. */
QString tupleKey(const QStringList& parts) {
  return QString::fromLatin1(QJsonDocument(QJsonArray::fromStringList(parts))
      .toJson(QJsonDocument::Compact).toBase64(QByteArray::Base64UrlEncoding));
}

} // namespace

OneGenFeatureTree::OneGenFeatureTree(QWidget* parent, const QString& settingsFile,
                                   const QString& favoritesKey)
    : SearchableTreeWidget(parent,
          settingsFile.isEmpty()
              ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                    .filePath(QStringLiteral("OneGenFeatures.ini"))
              : settingsFile,
          favoritesKey.isEmpty() ? QStringLiteral("Favorites/Nodes") : favoritesKey) {
  setSearchPlaceholderText(tr("Search features"));
}

void OneGenFeatureTree::registerFeature(QTreeWidgetItem* item, const QString& moduleName) {
  if (!item) return;
  registerItem(item, tupleKey({moduleName, item->data(0, NodeNameRole).toString()}));
}

QString OneGenFeatureTree::itemStateKey(const QTreeWidgetItem* item) const {
  return tupleKey({item->data(0, MapRole).toString(), item->data(0, NodeNameRole).toString()});
}

QString OneGenFeatureTree::itemSearchText(const QTreeWidgetItem* item) const {
  return item->text(0) + QChar(' ') + item->data(0, NodeNameRole).toString();
}
