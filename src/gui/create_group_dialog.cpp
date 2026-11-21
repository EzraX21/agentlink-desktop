#include "create_group_dialog.hpp"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

namespace agentlink {

CreateGroupDialog::CreateGroupDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle(QStringLiteral("建群"));
  setModal(true);

  auto* layout = new QVBoxLayout(this);
  auto* form = new QFormLayout();

  titleEdit_ = new QLineEdit(this);
  titleEdit_->setPlaceholderText(QStringLiteral("群名（可选，默认「群聊」）"));

  membersEdit_ = new QLineEdit(this);
  membersEdit_->setPlaceholderText(QStringLiteral("成员 ID，逗号分隔（可选，例如 u_xxx,u_yyy）"));

  form->addRow(QStringLiteral("群名"), titleEdit_);
  form->addRow(QStringLiteral("成员"), membersEdit_);
  layout->addLayout(form);

  auto* hint = new QLabel(
      QStringLiteral("成员 ID 是 u_ 开头的字符串，可以先从管理页 / 对方那里拿到。"),
      this);
  hint->setWordWrap(true);
  hint->setStyleSheet(QStringLiteral("color:#888;"));
  layout->addWidget(hint);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);
}

QString CreateGroupDialog::title() const {
  return titleEdit_->text().trimmed();
}

QStringList CreateGroupDialog::memberIds() const {
  QStringList ids;
  const QString raw = membersEdit_->text().trimmed();
  if (raw.isEmpty()) return ids;
  for (const QString& part : raw.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
    const QString id = part.trimmed();
    if (!id.isEmpty()) ids << id;
  }
  return ids;
}

}  // namespace agentlink
