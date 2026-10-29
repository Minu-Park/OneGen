#include "OneGenDialogs.h"

#include <QByteArray>
#include <QCheckBox>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>

#include <exception>
#include <stdexcept>
#include <utility>

OneGenDialogShell createOneGenDialogShell(const OneGenDialogShellFactory& factory,
                                          QWidget* parent, const QString& title)
{
  if (factory) {
    auto shell = factory(parent, title);
    if (!shell.dialog || !shell.content)
      throw std::runtime_error("The dialog shell factory returned no dialog or content layout.");
    return shell;
  }
  // Standalone plain-Qt fallback: no consumer chrome is installed.
  OneGenDialogShell shell{std::make_unique<QDialog>(parent), nullptr};
  shell.dialog->setWindowTitle(title);
  shell.content = new QVBoxLayout(shell.dialog.get());
  return shell;
}

void populateOneGenRegisterInspector(QDialog& dialog, QVBoxLayout* content,
                                     OneGenRegisterInspectorRequest request)
{
  const auto state = std::make_shared<OneGenRegisterInspectorRequest>(std::move(request));
  auto* bytes = new QPlainTextEdit;
  bytes->setObjectName(QStringLiteral("OneGenRegisterBytes"));
  bytes->setReadOnly(true);
  if (state->value) {
    const auto& value = *state->value;
    bytes->setPlainText(QString::fromLatin1(QByteArray(
        reinterpret_cast<const char*>(value.data()), static_cast<qsizetype>(value.size())).toHex(' ')));
  }
  auto* allow = new QCheckBox(QStringLiteral("Enable raw register writes"));
  allow->setObjectName(QStringLiteral("OneGenRegisterWriteConsent"));
  allow->setEnabled(state->writeAllowed);
  auto* actions = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
  actions->button(QDialogButtonBox::Save)->setEnabled(false);
  QObject::connect(allow, &QCheckBox::toggled, &dialog, [bytes, actions](bool checked) {
    bytes->setReadOnly(!checked);
    actions->button(QDialogButtonBox::Save)->setEnabled(checked);
  });
  content->addWidget(new QLabel(
      QStringLiteral("%1 bytes. Raw writes bypass named feature controls.").arg(state->length)));
  content->addWidget(bytes);
  content->addWidget(allow);
  content->addWidget(actions);
  QObject::connect(actions, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  QObject::connect(actions, &QDialogButtonBox::accepted, &dialog, [&dialog, bytes, allow, state] {
    if (!allow->isChecked()) return;
    auto hex = bytes->toPlainText();
    hex.remove(QRegularExpression(QStringLiteral("\\s+")));
    if (hex.size() != static_cast<qsizetype>(state->length * 2)
        || !QRegularExpression(QStringLiteral("\\A[0-9a-fA-F]+\\z")).match(hex).hasMatch()) {
      qWarning() << "[GenICam] register write requires the exact number of hexadecimal bytes";
      return;
    }
    const auto raw = QByteArray::fromHex(hex.toLatin1());
    try {
      state->write(std::vector<std::uint8_t>(raw.begin(), raw.end()));
      qInfo().noquote() << "[GenICam] raw register written:" << state->name;
      dialog.accept();
    } catch (const std::exception& error) {
      qWarning().noquote() << "[GenICam] register write failed:" << error.what();
    }
  });
}
