#include "MainWindow.hpp"

#include "FeatureTreeWidget.hpp"

#include "onegen/error.hpp"

#include <exception>
#include <string>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  auto* root = new QWidget;
  auto* layout = new QVBoxLayout(root);

  auto* cti_row = new QHBoxLayout;
  cti_path_ = new QLineEdit;
  cti_path_->setPlaceholderText(QString::fromStdString(onegen::defaults::pylonCti().string()));
  auto* load = new QPushButton("LoadCTI");
  cti_row->addWidget(new QLabel("CTI"));
  cti_row->addWidget(cti_path_, 1);
  cti_row->addWidget(load);
  layout->addLayout(cti_row);

  auto* cam_row = new QHBoxLayout;
  cams_ = new QComboBox;
  query_ = new QLineEdit;
  query_->setPlaceholderText("192.168.0.157");
  query_->setText("192.168.0.157");
  auto* add = new QPushButton("addCamera");
  auto* open = new QPushButton("open");
  auto* close = new QPushButton("close");
  cam_row->addWidget(add);
  cam_row->addWidget(cams_);
  cam_row->addWidget(query_, 1);
  cam_row->addWidget(open);
  cam_row->addWidget(close);
  layout->addLayout(cam_row);

  auto* acq_row = new QHBoxLayout;
  n_single_ = new QSpinBox;
  n_single_->setMinimum(1);
  n_single_->setMaximum(1000);
  n_single_->setValue(1);
  auto* single = new QPushButton("single");
  auto* live = new QPushButton("live");
  auto* stop = new QPushButton("stop");
  acq_row->addWidget(n_single_);
  acq_row->addWidget(single);
  acq_row->addWidget(live);
  acq_row->addWidget(stop);
  acq_row->addStretch();
  layout->addLayout(acq_row);

  tree_ = new FeatureTreeWidget;
  layout->addWidget(tree_, 1);

  status_ = new QLabel;
  layout->addWidget(status_);
  setCentralWidget(root);
  resize(900, 700);
  setWindowTitle("onegen");

  connect(load, &QPushButton::clicked, this, &MainWindow::loadCti);
  connect(add, &QPushButton::clicked, this, &MainWindow::addCam);
  connect(open, &QPushButton::clicked, this, &MainWindow::openCam);
  connect(close, &QPushButton::clicked, this, &MainWindow::closeCam);
  connect(single, &QPushButton::clicked, this, &MainWindow::doSingle);
  connect(live, &QPushButton::clicked, this, &MainWindow::doLive);
  connect(stop, &QPushButton::clicked, this, &MainWindow::doStop);
  connect(cams_, &QComboBox::currentIndexChanged, this, [this] {
    auto* c = current();
    tree_->setNodeMap(c && c->is_open() ? &c->features() : nullptr,
                      c && c->is_open() ? (c->info().display_name.empty() ? c->info().id
                                                                          : c->info().display_name)
                                        : std::string{});
    refreshStatus();
  });
  refreshStatus();
}

onegen::Camera* MainWindow::current() {
  int i = cams_->currentIndex();
  if (i < 0 || i >= static_cast<int>(og_.cameras().size())) return nullptr;
  return og_.cameras()[static_cast<size_t>(i)];
}

void MainWindow::refreshStatus() {
  status_->setText(QString("cameras=%1 frames=%2").arg(og_.cameras().size()).arg(frames_));
}

void MainWindow::loadCti() {
  try {
    auto p = cti_path_->text().toStdString();
    if (p.empty()) p = onegen::defaults::pylonCti().string();
    if (p.empty()) throw onegen::Error("CTI path empty — set path explicitly");
    og_.loadCti(p);
    refreshStatus();
  } catch (const std::exception& e) {
    QMessageBox::warning(this, "loadCti", e.what());
  }
}

void MainWindow::addCam() {
  og_.addCamera();
  cams_->addItem(QString("cam %1").arg(og_.cameras().size() - 1));
  cams_->setCurrentIndex(cams_->count() - 1);
  refreshStatus();
}

void MainWindow::openCam() {
  auto* c = current();
  if (!c) {
    addCam();
    c = current();
  }
  try {
    c->open(query_->text().toStdString());
    const auto& inf = c->info();
    const auto title = inf.display_name.empty() ? inf.id : inf.display_name;
    tree_->setNodeMap(&c->features(), title);
    cams_->setItemText(cams_->currentIndex(), QString::fromStdString(title));
    refreshStatus();
  } catch (const std::exception& e) {
    QMessageBox::warning(this, "open", e.what());
  }
}

void MainWindow::closeCam() {
  auto* c = current();
  if (!c) return;
  try {
    c->close();
    tree_->setGrabbing(false);
    tree_->setNodeMap(nullptr);
    refreshStatus();
  } catch (const std::exception& e) {
    QMessageBox::warning(this, "close", e.what());
  }
}

void MainWindow::doSingle() {
  auto* c = current();
  if (!c) return;
  try {
    auto frames = c->single(static_cast<size_t>(n_single_->value()));
    frames_ += static_cast<int>(frames.size());
    if (!frames.empty())
      status_->setText(QString("single %1x%2 pxfmt=%3 n=%4 bytes=%5")
                           .arg(frames.back().width)
                           .arg(frames.back().height)
                           .arg(frames.back().pixel_format)
                           .arg(frames.size())
                           .arg(frames.back().payload.size()));
  } catch (const std::exception& e) {
    QMessageBox::warning(this, "single", e.what());
  }
}

void MainWindow::doLive() {
  auto* c = current();
  if (!c) return;
  try {
    c->set_frame_callback([this](const onegen::Frame&) { ++frames_; });
    c->live();
    tree_->setGrabbing(true);
  } catch (const std::exception& e) {
    tree_->setGrabbing(false);
    QMessageBox::warning(this, "live", e.what());
  }
}

void MainWindow::doStop() {
  auto* c = current();
  if (!c) return;
  try {
    c->stop();
    tree_->setGrabbing(false);
    refreshStatus();
  } catch (const std::exception& e) {
    QMessageBox::warning(this, "stop", e.what());
  }
}
