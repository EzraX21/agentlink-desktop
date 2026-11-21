#pragma once

#include <QDialog>

class QLineEdit;
class QPlainTextEdit;

namespace agentlink {

/**
 * 群信息对话框：改名 + 公告。
 *
 * 只有群主 / 管理员能保存（服务端 group.update 有权限校验，改了也白改，
 * 但客户端也顺手把按钮藏起来，见 MainWindow 里的 canEditGroup）。
 */
class GroupInfoDialog : public QDialog {
  Q_OBJECT

 public:
  GroupInfoDialog(const QString& initialTitle, const QString& initialAnnouncement,
                  QWidget* parent = nullptr);

  [[nodiscard]] QString title() const;
  [[nodiscard]] QString announcement() const;

 private:
  QLineEdit* titleEdit_ = nullptr;
  QPlainTextEdit* announcementEdit_ = nullptr;
};

}  // namespace agentlink
