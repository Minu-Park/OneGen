#include "OneGenControlUi.h"
#include <QtWidgets>
#include "OneGenFeatureTree.h"


void OneGenControlUi::create(QWidget* owner, const QString& settingsFile, const QString& favoritesKey) {
    owner->setProperty("actionDensity", "compact");
    owner->setProperty("controlRole", "devicePanel");

  owner->setMinimumSize(300, 350);
  auto* layout = new QVBoxLayout(owner);
  layout->setObjectName(QStringLiteral("DeviceRootLayout"));
  layout->setProperty("layoutRole", "flush");

  devices_box_ = new QComboBox;
  devices_box_->setMinimumWidth(120);
  devices_box_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  devices_box_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  streams_box_ = new QComboBox;
  streams_box_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  streams_box_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  streams_box_->setToolTip(QStringLiteral("Data Stream"));
  streams_box_->setEnabled(false);

  refresh_button_ = new QToolButton;
  refresh_button_->setIcon(QIcon(QStringLiteral(":/Resources/icon/refresh-48.png")));
  refresh_button_->setIconSize(QSize(16, 16));
  refresh_button_->setToolButtonStyle(Qt::ToolButtonIconOnly);
  refresh_button_->setToolTip(QStringLiteral("Refresh"));

  connect_button_ = new QToolButton;
  QIcon connect_icon;
  // The glyph names the current state; the shared theme previews the other state on hover.
  connect_icon.addFile(QStringLiteral(":/Resources/icon/disconnected-48.png"), {},
                       QIcon::Normal, QIcon::Off);
  connect_icon.addFile(QStringLiteral(":/Resources/icon/connect-48.png"), {},
                       QIcon::Normal, QIcon::On);
  connect_button_->setIcon(connect_icon);
  connect_button_->setIconSize(QSize(16, 16));
  connect_button_->setToolButtonStyle(Qt::ToolButtonIconOnly);
  connect_button_->setCheckable(true);
  connect_button_->setToolTip(QStringLiteral("Connect / Disconnect"));

  grab_one_button_ = new QToolButton;
  grab_one_button_->setIcon(QIcon(QStringLiteral(":/Resources/icon/camera-48.png")));
  grab_one_button_->setIconSize(QSize(16, 16));
  grab_one_button_->setToolButtonStyle(Qt::ToolButtonIconOnly);
  grab_one_button_->setToolTip(QStringLiteral("Grab One"));
  grab_one_button_->setEnabled(false);

  grab_live_button_ = new QToolButton;
  QIcon live_icon;
  // Paused while stopped, live while grabbing.
  live_icon.addFile(QStringLiteral(":/Resources/icon/pause-48.png"), {},
                    QIcon::Normal, QIcon::Off);
  live_icon.addFile(QStringLiteral(":/Resources/icon/cameras-48.png"), {},
                    QIcon::Normal, QIcon::On);
  grab_live_button_->setIcon(live_icon);
  grab_live_button_->setIconSize(QSize(16, 16));
  grab_live_button_->setToolButtonStyle(Qt::ToolButtonIconOnly);
  grab_live_button_->setToolTip(QStringLiteral("Grab Live / Stop"));
  grab_live_button_->setCheckable(true);
  grab_live_button_->setEnabled(false);

  auto* selector_layout = new QHBoxLayout;
  selector_layout->setObjectName(QStringLiteral("DeviceSelectorLayout"));
  selector_layout->setProperty("layoutRole", "row");
  selector_layout->addWidget(devices_box_);
  selector_layout->addWidget(streams_box_);
  selector_layout->addWidget(refresh_button_);
  auto* tool_layout = new QHBoxLayout;
  tool_layout->setObjectName(QStringLiteral("DeviceToolLayout"));
  tool_layout->setProperty("layoutRole", "actions");
  tool_layout->addWidget(connect_button_);
  tool_layout->addWidget(grab_one_button_);
  tool_layout->addWidget(grab_live_button_);
  auto* top_layout = new QHBoxLayout;
  top_layout->setObjectName(QStringLiteral("DeviceTopBarLayout"));
  top_layout->setProperty("layoutRole", "content");
  top_layout->addLayout(selector_layout, 1);
  top_layout->addLayout(tool_layout, 0);
  layout->addLayout(top_layout);

  features_ = new OneGenFeatureTree(nullptr, settingsFile, favoritesKey);
  features_->setObjectName(QStringLiteral("CameraFeaturesTree"));
  features_->setProperty("treeRole", QStringLiteral("DeviceFeatureTree"));
  features_->setColumnCount(2);
  features_->setHeaderLabels({QStringLiteral("Feature"), QStringLiteral("Value")});
  features_->setUniformRowHeights(true);
  features_->setRootIsDecorated(true);
  features_->setContextMenuPolicy(Qt::CustomContextMenu);

  auto* features_layout = new QVBoxLayout;
  features_layout->setObjectName(QStringLiteral("DeviceTreePanelLayout"));
  features_layout->setProperty("layoutRole", "bottom-panel");
  features_layout->addWidget(features_);
  layout->addLayout(features_layout);

  auto* status_bar = new QStatusBar;
  status_bar->setObjectName(QStringLiteral("CameraStatusBar"));
  status_bar->setSizeGripEnabled(false);
  status_ = new QLabel(QStringLiteral("Disconnected"));
  status_->setObjectName(QStringLiteral("CameraStatusLabel"));
  status_->setAlignment(Qt::AlignCenter);
  status_bar->addWidget(status_);
  layout->addWidget(status_bar);


}
