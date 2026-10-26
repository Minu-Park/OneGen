#pragma once
#include <QWidget>
#include <QString>
class QComboBox; class QToolButton; class QTreeWidget; class QStatusBar; class QLabel;
class QLineEdit; class QTabWidget; class OneGenFeatureTree;

/** @brief Owns the device control composition without SDK or acquisition dependencies. */
struct OneGenControlUi {
    /** @brief Creates the same controls used by the runtime widget. */
    void create(QWidget* owner, const QString& settingsFile = {}, const QString& favoritesKey = {});
    QComboBox* devices_box_ = nullptr;
    QComboBox* streams_box_ = nullptr;
    QToolButton* refresh_button_ = nullptr;
    QToolButton* connect_button_ = nullptr;
    QToolButton* grab_one_button_ = nullptr;
    QToolButton* grab_live_button_ = nullptr;
    QLabel* status_ = nullptr;
    OneGenFeatureTree* features_ = nullptr;
};
