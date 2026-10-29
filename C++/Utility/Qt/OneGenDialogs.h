#pragma once

#include <QString>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QDialog;
class QVBoxLayout;
class QWidget;

/** @brief Modal dialog window and the layout that receives OneGen-owned content. */
struct OneGenDialogShell {
  std::unique_ptr<QDialog> dialog;
  /** Layout owned by @ref dialog; OneGen adds its content here instead of replacing the shell layout. */
  QVBoxLayout* content = nullptr;
};

/**
 * @brief Creates a consumer-styled dialog shell for OneGen content.
 *
 * Consumers install this from their adapter to supply their own window chrome. The callback
 * runs on the GUI thread and must return a dialog parented to @p parent and its content layout.
 */
using OneGenDialogShellFactory =
    std::function<OneGenDialogShell(QWidget* parent, const QString& title)>;

/**
 * @brief Creates the consumer shell, or a plain Qt dialog when no factory is installed.
 * @throws std::runtime_error when an installed factory returns no dialog or content layout.
 */
OneGenDialogShell createOneGenDialogShell(const OneGenDialogShellFactory& factory,
                                          QWidget* parent, const QString& title);

/** @brief Inputs for the raw register editor; the caller performs register access. */
struct OneGenRegisterInspectorRequest {
  QString name;
  std::size_t length = 0;
  /** Current bytes, or empty when the register is not readable. */
  std::optional<std::vector<std::uint8_t>> value;
  /** Whether the explicit raw-write consent may be given. */
  bool writeAllowed = false;
  /** Writes exactly @ref length bytes; throws on failure so the dialog stays open. */
  std::function<void(const std::vector<std::uint8_t>&)> write;
};

/**
 * @brief Adds the raw register editor to @p content and accepts @p dialog after a successful write.
 *
 * Writes require explicit consent and exactly @c length hexadecimal bytes. Invalid input and
 * write failures are logged and leave the dialog open.
 */
void populateOneGenRegisterInspector(QDialog& dialog, QVBoxLayout* content,
                                     OneGenRegisterInspectorRequest request);
