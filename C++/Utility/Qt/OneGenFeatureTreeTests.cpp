#include "OneGenFeatureTree.h"
#include "QOneGenWidget.h"
#ifdef ONEGEN_FEATURE_THEME
#include "Resources.h"
#endif

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QFrame>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QScrollBar>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTest>
#include <QToolButton>
#include <QUuid>

namespace {

void configure(OneGenFeatureTree& tree) {
  tree.setObjectName(QStringLiteral("CameraFeaturesTree"));
  tree.setProperty("treeRole", QStringLiteral("DeviceFeatureTree"));
  tree.setColumnCount(2);
  tree.setHeaderLabels({QStringLiteral("Feature"), QStringLiteral("Value")});
  tree.resize(400, 400);
}

QTreeWidgetItem* group(QTreeWidgetItem* parent, const QString& name, int map = 0) {
  auto* item = new QTreeWidgetItem(parent, {name});
  item->setData(0, OneGenFeatureTree::NodeNameRole, name);
  item->setData(0, OneGenFeatureTree::MapRole, map);
  return item;
}

QTreeWidgetItem* leaf(OneGenFeatureTree& tree, QTreeWidgetItem* parent,
                      const QString& name, const QString& display, const QString& module,
                      int map = 0) {
  auto* item = new QTreeWidgetItem(parent, {display});
  item->setData(0, OneGenFeatureTree::NodeNameRole, name);
  item->setData(0, OneGenFeatureTree::MapRole, map);
  auto* editor = new QLineEdit(QStringLiteral("42"));
  tree.setItemWidget(item, 1, editor);
  tree.registerFeature(item, module);
  return item;
}

onegen::FeatureNode snapshot(bool present, bool enabled) {
  onegen::FeatureNode root;
  root.name = "Root";
  root.kind = onegen::NodeKind::Category;
  onegen::FeatureNode category;
  category.name = "AcquisitionControl";
  category.display_name = "Acquisition Control";
  category.kind = onegen::NodeKind::Category;
  if (present) {
    onegen::FeatureNode node;
    node.name = "TriggerEnabled";
    node.display_name = "Trigger Enabled";
    node.kind = onegen::NodeKind::Boolean;
    node.readable = node.writable = node.visible = true;
    node.bool_value = enabled;
    category.children.push_back(node);
  }
  root.children.push_back(category);
  return root;
}

} // namespace

class FeatureTreeTests final : public QObject {
  Q_OBJECT
private slots:
  void init() {
    QSettings settings(QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("OneGenFeatures.ini")), QSettings::IniFormat);
    settings.clear();
    settings.sync();
  }

  void searchPreservesEditorsAndExpansion() {
    OneGenFeatureTree tree;
    configure(tree);
    auto* root = new QTreeWidgetItem(&tree, {QStringLiteral("Camera")});
    auto* acquisition = group(root, QStringLiteral("AcquisitionControl"));
    auto* analog = group(root, QStringLiteral("AnalogControl"));
    auto* exposure = leaf(tree, acquisition, QStringLiteral("ExposureTime"),
        QStringLiteral("Exposure Time"), QStringLiteral("Remote Device"));
    auto* gain = leaf(tree, analog, QStringLiteral("GainRaw"), QStringLiteral("Gain"),
        QStringLiteral("Remote Device"));
    root->setExpanded(true);
    acquisition->setExpanded(false);
    analog->setExpanded(true);
    auto* editor = tree.itemWidget(exposure, 1);
    tree.show();
    auto* input = tree.findChild<QLineEdit*>(QStringLiteral("DeviceFeatureSearchInput"));
    input->setText(QStringLiteral("eXpOsUrEtImE"));
    QVERIFY(!exposure->isHidden());
    QVERIFY(gain->isHidden());
    QVERIFY(analog->isHidden());
    QVERIFY(acquisition->isExpanded());
    QCOMPARE(tree.itemWidget(exposure, 1), editor);
    input->setText(QStringLiteral("AcquisitionControl Exposure"));
    QVERIFY(!exposure->isHidden());
    input->setText(QStringLiteral("AnalogControl"));
    QVERIFY(!gain->isHidden());
    QVERIFY(exposure->isHidden());
    input->setText(QStringLiteral("no-such-feature"));
    QCOMPARE(tree.findChild<QLabel*>(QStringLiteral("DeviceFeatureSearchCount"))->text(),
             QStringLiteral("No results"));
    input->clear();
    QVERIFY(!gain->isHidden() && !exposure->isHidden());
    QVERIFY(!acquisition->isExpanded());
    QVERIFY(analog->isExpanded());
    QCOMPARE(tree.itemWidget(exposure, 1), editor);
  }

  void favoritesPersistAndShareAcrossDevices() {
    OneGenFeatureTree otherDevice;
    configure(otherDevice);
    auto* otherRoot = new QTreeWidgetItem(&otherDevice, {QStringLiteral("Other device")});
    auto* shared = leaf(otherDevice, otherRoot, QStringLiteral("Mode"), QStringLiteral("Other display"),
                         QStringLiteral("Remote Device"));
    {
      OneGenFeatureTree tree;
      configure(tree);
      auto* root = new QTreeWidgetItem(&tree, {QStringLiteral("Camera")});
      auto* remote = leaf(tree, root, QStringLiteral("Mode"), QStringLiteral("Mode"),
                          QStringLiteral("Remote Device"));
      auto* stream = leaf(tree, root, QStringLiteral("Mode"), QStringLiteral("Mode"),
                          QStringLiteral("Data Stream"), 1);
      tree.toggleFavorite(remote);
      QVERIFY(tree.isFavorite(remote));
      QVERIFY(otherDevice.isFavorite(shared));
      QVERIFY(shared->data(0, OneGenFeatureTree::FavoriteRole).toBool());
      QVERIFY(!tree.isFavorite(stream));
      auto* favoriteFilter = tree.findChild<QToolButton*>(QStringLiteral("DeviceFeatureFavoritesOnly"));
      favoriteFilter->setChecked(true);
      QVERIFY(!remote->isHidden());
      QVERIFY(stream->isHidden());
      tree.findChild<QLineEdit*>(QStringLiteral("DeviceFeatureSearchInput"))->setText(QStringLiteral("absent"));
      QVERIFY(remote->isHidden());
      tree.findChild<QLineEdit*>(QStringLiteral("DeviceFeatureSearchInput"))->clear();
      QVERIFY(!remote->isHidden());
      // An absent node is simply omitted; the shared saved favorite remains.
      otherDevice.clear();
      otherDevice.refreshFilter();
      QCOMPARE(otherDevice.topLevelItemCount(), 0);
    }
    OneGenFeatureTree reopened;
    configure(reopened);
    auto* root = new QTreeWidgetItem(&reopened, {QStringLiteral("Renamed camera")});
    auto* renamed = leaf(reopened, root, QStringLiteral("Mode"), QStringLiteral("Renamed display"),
                         QStringLiteral("Remote Device"));
    QVERIFY(reopened.isFavorite(renamed));
    reopened.toggleFavorite(renamed);
    QVERIFY(!reopened.isFavorite(renamed));
  }

  void snapshotsKeepEditorsAndUnavailableFavorites() {
    QOneGenWidget panel({}, nullptr, {}, {});
    onegen::InterfaceInfo identity;
    identity.cti_path = QUuid::createUuid().toString().toStdString();
    identity.id = "interface";
    identity.display_name = "Interface Settings";
    panel.setInterface(identity, snapshot(true, false));
    auto* tree = panel.findChild<OneGenFeatureTree*>();
    auto* root = tree->topLevelItem(0);
    auto* category = root->child(0);
    auto* item = category->child(0);
    auto* editor = tree->itemWidget(item, 1);
    tree->toggleFavorite(item);
    tree->findChild<QToolButton*>(QStringLiteral("DeviceFeatureFavoritesOnly"))->setChecked(true);
    tree->findChild<QLineEdit*>(QStringLiteral("DeviceFeatureSearchInput"))->setText(QStringLiteral("trigger"));
    panel.setInterface(identity, snapshot(true, true));
    QCOMPARE(tree->topLevelItem(0), root);
    QCOMPARE(root->child(0), category);
    QCOMPARE(category->child(0), item);
    QCOMPARE(tree->itemWidget(item, 1), editor);
    QVERIFY(qobject_cast<QCheckBox*>(editor)->isChecked());
    QVERIFY(tree->isFavorite(item));
    QVERIFY(!item->isHidden());
    panel.setInterface(identity, snapshot(false, false));
    QCOMPARE(root->childCount(), 0);
    panel.setInterface(identity, snapshot(true, false));
    auto* returned = root->child(0)->child(0);
    QVERIFY(tree->isFavorite(returned));
    QVERIFY(!returned->isHidden());
    tree->toggleFavorite(returned);
  }

  void differentCameraModelsShareExposureTime() {
    OneGenFeatureTree aca1300;
    OneGenFeatureTree aca2500;
    OneGenFeatureTree missing;
    configure(aca1300);
    configure(aca2500);
    configure(missing);
    auto* root1300 = new QTreeWidgetItem(&aca1300, {QStringLiteral("aca1300")});
    auto* root2500 = new QTreeWidgetItem(&aca2500, {QStringLiteral("aca2500")});
    auto* rootMissing = new QTreeWidgetItem(&missing, {QStringLiteral("Other model")});
    auto* exposure1300 = leaf(aca1300, root1300, QStringLiteral("ExposureTime"),
        QStringLiteral("Exposure Time"), QStringLiteral("Remote Device"));
    auto* exposure2500 = leaf(aca2500, root2500, QStringLiteral("ExposureTime"),
        QStringLiteral("Exposure Time"), QStringLiteral("Remote Device"));
    auto* gain = leaf(missing, rootMissing, QStringLiteral("Gain"),
        QStringLiteral("Gain"), QStringLiteral("Remote Device"));
    aca1300.toggleFavorite(exposure1300);
    for (auto* tree : {&aca1300, &aca2500, &missing})
      tree->findChild<QToolButton*>(QStringLiteral("DeviceFeatureFavoritesOnly"))->setChecked(true);
    QVERIFY(aca2500.isFavorite(exposure2500));
    QVERIFY(!exposure1300->isHidden() && !exposure2500->isHidden());
    QVERIFY(gain->isHidden() && rootMissing->isHidden());
    auto* laterExposure = leaf(missing, rootMissing, QStringLiteral("ExposureTime"),
        QStringLiteral("Exposure Time"), QStringLiteral("Remote Device"));
    missing.refreshFilter();
    QVERIFY(missing.isFavorite(laterExposure));
    QVERIFY(!laterExposure->isHidden());
  }

  void keyboardAndRowStar() {
    OneGenFeatureTree tree;
    configure(tree);
    auto* root = new QTreeWidgetItem(&tree, {QStringLiteral("Camera")});
    auto* item = leaf(tree, root, QStringLiteral("ExposureTime"), QStringLiteral("Exposure"),
                      QStringLiteral("Remote Device"));
    root->setExpanded(true);
    tree.show();
    tree.activateWindow();
    tree.setFocus();
    QTest::qWait(60);
    auto* input = tree.findChild<QLineEdit*>(QStringLiteral("DeviceFeatureSearchInput"));
    QTest::keyClick(&tree, Qt::Key_F, Qt::ControlModifier);
    QTRY_VERIFY(input->hasFocus());
    input->setText(QStringLiteral("Exposure"));
    QTest::keyClick(input, Qt::Key_Escape);
    QVERIFY(input->text().isEmpty());
    tree.setCurrentItem(item);
    tree.setFocus();
    QTest::keyClick(&tree, Qt::Key_D, Qt::ControlModifier);
    QVERIFY(tree.isFavorite(item));
    const auto rect = tree.visualItemRect(item);
    const int nameRight = tree.columnWidth(0) - tree.horizontalScrollBar()->value();
    const int extent = tree.style()->pixelMetric(QStyle::PM_SmallIconSize);
    QTest::mouseClick(tree.viewport(), Qt::LeftButton, Qt::NoModifier,
        QPoint(nameRight - extent / 2 - 4, rect.center().y()));
    QVERIFY(!tree.isFavorite(item));
  }

  void clearSearchAlignmentAndBehavior() {
    OneGenFeatureTree tree;
    configure(tree);
    auto* root = new QTreeWidgetItem(&tree, {QStringLiteral("Camera")});
    auto* exposure = leaf(tree, root, QStringLiteral("ExposureTime"), QStringLiteral("Exposure Time"),
                           QStringLiteral("Remote Device"));
    root->setExpanded(true);
    auto* editor = tree.itemWidget(exposure, 1);
    auto* input = tree.findChild<QLineEdit*>(QStringLiteral("DeviceFeatureSearchInput"));
    auto* clear = tree.findChild<QToolButton*>(QStringLiteral("DeviceFeatureSearchClear"));
    auto* count = tree.findChild<QLabel*>(QStringLiteral("DeviceFeatureSearchCount"));
    auto* favorites = tree.findChild<QToolButton*>(QStringLiteral("DeviceFeatureFavoritesOnly"));
    auto* bubble = tree.findChild<QFrame*>(QStringLiteral("DeviceFeatureSearchBubble"));
    QVERIFY(clear);
    tree.show();
    tree.activateWindow();
    QTest::qWait(50);
    QVERIFY(!clear->isVisible());
    for (int width : {300, 460, 900}) {
      tree.resize(width, 400);
      input->setFocus();
      input->setText(QStringLiteral("exal"));
      QTest::qWait(30);
      QVERIFY(clear->isVisible());
      QCOMPARE(count->text(), QStringLiteral("No results"));
      const auto bounds = [bubble](QWidget* widget) {
        return QRect(widget->mapTo(bubble, QPoint()), widget->size());
      };
      QVERIFY(bubble->rect().contains(bounds(clear)));
      QVERIFY(qAbs(bounds(clear).center().y() - bounds(input).center().y()) <= 1);
      QVERIFY(qAbs(bounds(clear).center().y() - bounds(count).center().y()) <= 1);
      QVERIFY(qAbs(bounds(clear).center().y() - bounds(favorites).center().y()) <= 1);
      QVERIFY(!clear->icon().isNull());
      const QString capturePath = qEnvironmentVariable("ONEGEN_FEATURE_CLEAR_CAPTURE");
      if (!capturePath.isEmpty() && width == 460) {
        const QRect region = bubble->geometry().adjusted(-12, -12, 12, 12).intersected(tree.viewport()->rect());
        QVERIFY(tree.viewport()->grab(region).save(capturePath));
      }
      QTest::mouseClick(clear, Qt::LeftButton);
      QVERIFY(input->text().isEmpty());
      QVERIFY(input->hasFocus());
      QVERIFY(!clear->isVisible());
      QVERIFY(!count->isVisible());
      QVERIFY(!exposure->isHidden());
      QCOMPARE(tree.itemWidget(exposure, 1), editor);
    }
  }

  void overlayClearanceAndScrollRestoration() {
    OneGenFeatureTree tree;
    configure(tree);
    auto* root = new QTreeWidgetItem(&tree, {QStringLiteral("Camera")});
    QTreeWidgetItem* last = nullptr;
    for (int i = 0; i < 50; ++i)
      last = leaf(tree, root, QStringLiteral("Exposure%1").arg(i),
                  QStringLiteral("Exposure %1").arg(i), QStringLiteral("Remote Device"));
    root->setExpanded(true);
    tree.show();
    for (const auto size : {QSize(276, 240), QSize(400, 400), QSize(800, 700)}) {
      tree.resize(size);
      QTest::qWait(60);
      tree.scrollToBottom();
      QTest::qWait(60);
      const int bottomScroll = tree.verticalScrollBar()->value();
      tree.refreshFilter();
      QCOMPARE(tree.verticalScrollBar()->value(), bottomScroll);
      auto* bubble = tree.findChild<QFrame*>(QStringLiteral("DeviceFeatureSearchBubble"));
      QVERIFY2(tree.viewport()->rect().contains(bubble->geometry()),
          qPrintable(QStringLiteral("Viewport %1x%2, bubble %3,%4 %5x%6")
          .arg(tree.viewport()->width()).arg(tree.viewport()->height())
          .arg(bubble->x()).arg(bubble->y()).arg(bubble->width()).arg(bubble->height())));
      QVERIFY2(tree.visualItemRect(last).bottom() < bubble->y(), "Final feature obscured by search bubble");
      QVERIFY(!tree.findChild<QLabel*>(QStringLiteral("DeviceFeatureSearchBackdrop")));
#ifdef ONEGEN_FEATURE_THEME
      if (!qEnvironmentVariableIsSet("ONEGEN_FEATURE_PLAIN")) {
        QLabel reference(QStringLiteral("Idle"));
        reference.setObjectName(QStringLiteral("CameraStatusLabel"));
        reference.ensurePolished();
        reference.resize(reference.sizeHint());
        QCOMPARE(bubble->height(), reference.height());
      }
#endif
      auto* input = tree.findChild<QLineEdit*>(QStringLiteral("DeviceFeatureSearchInput"));
      QVERIFY(input->width() > 50);
      tree.verticalScrollBar()->setValue(tree.verticalScrollBar()->maximum() / 2);
      const int before = tree.verticalScrollBar()->value();
      input->setText(QStringLiteral("Exposure49"));
      input->clear();
      QCOMPARE(tree.verticalScrollBar()->value(), before);
    }
    tree.resize(460, 450);
    tree.header()->resizeSection(0, 220);
    tree.toggleFavorite(root->child(16));
    tree.toggleFavorite(root->child(20));
    tree.verticalScrollBar()->setValue(tree.verticalScrollBar()->maximum() / 3);
    QTest::qWait(100);
    tree.grab().save(QStringLiteral("onegen-feature-search.png"));
  }
};

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("OneGenTests"));
  QCoreApplication::setApplicationName(QStringLiteral("FeatureTreeTests"));
  QStandardPaths::setTestModeEnabled(true);
#ifdef ONEGEN_FEATURE_THEME
  if (!qEnvironmentVariableIsSet("ONEGEN_FEATURE_PLAIN")) Resources::installResources(app);
#endif
  FeatureTreeTests tests;
  return QTest::qExec(&tests, argc, argv);
}

#include "OneGenFeatureTreeTests.moc"
