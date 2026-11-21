#include "group_info_dialog.hpp"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QVBoxLayout>

namespace agentlink {

GroupInfoDialog::GroupInfoDialog(const QString& initialTitle, const QString& initialAnnouncement,
                                 QWidget* parent)
    : QDialog(parent) {
  setWindowTitle(QStringLiteral("群信息"));
  setModal(true);

  auto* layout = new QVBoxLayout(this);
  auto* form = new QFormLayout();

  titleEdit_ = new QLineEdit(initialTitle, this);
  announcementEdit_ = new QPlainTextEdit(initialAnnouncement, this);
  announcementEdit_->setPlaceholderText(QStringLiteral("群公告（可选，最多 4000 字）"));

  form->addRow(QStringLiteral("群名"), titleEdit_);
  form->addRow(QStringLiteral("公告"), announcementEdit_);
  layout->addLayout(form);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);
}

QString GroupInfoDialog::title() const {
  return titleEdit_->text().trimmed();
}

QString GroupInfoDialog::announcement() const {
  return announcementEdit_->toPlainText().trimmed();
}

}  // namespace agentlink
