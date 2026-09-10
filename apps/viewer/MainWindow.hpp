#pragma once

#include "onegen/onegen.hpp"

#include <QMainWindow>

class FeatureTreeWidget;
class QLineEdit;
class QComboBox;
class QSpinBox;
class QLabel;

class MainWindow : public QMainWindow {
  Q_OBJECT
public:
  explicit MainWindow(QWidget* parent = nullptr);

private:
  void loadCti();
  void addCam();
  void openCam();
  void closeCam();
  void doSingle();
  void doLive();
  void doStop();
  onegen::Camera* current();
  void refreshStatus();

  onegen::OneGen og_;
  QLineEdit* cti_path_ = nullptr;
  QLineEdit* query_ = nullptr;
  QComboBox* cams_ = nullptr;
  QSpinBox* n_single_ = nullptr;
  QLabel* status_ = nullptr;
  FeatureTreeWidget* tree_ = nullptr;
  int frames_ = 0;
};
