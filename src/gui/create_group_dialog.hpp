#pragma once

#include <QDialog>
#include <QStringList>

class QLineEdit;

namespace agentlink {

/**
 * 建群对话框：标题 + 成员 ID（逗号分隔，可选）。
 *
 * 成员 ID 是 `u_...` 这种 —— 用户从哪拿？暂时只能从管理页 / 对方那里问。
 * v1 不做"按昵称搜人"（那要一个 search.users 接口，先不做）。
 */
class CreateGroupDialog : public QDialog {
  Q_OBJECT

 public:
  explicit CreateGroupDialog(QWidget* parent = nullptr);

  [[nodiscard]] QString title() const;
  [[nodiscard]] QStringList memberIds() const;

 private:
  QLineEdit* titleEdit_ = nullptr;
  QLineEdit* membersEdit_ = nullptr;
};

}  // namespace agentlink
