#pragma once

#include <QDialog>
#include <QVariantList>
#include <QVariantMap>

class QListWidget;
class QPushButton;

namespace agentlink {

class Client;

/**
 * 成员管理对话框：列出成员 + 邀请 / 改名角色 / 禁言 / 踢人。
 *
 * 权限由服务端判定（`member.*` 都要求 owner/admin），所以这里不做本地拦截 ——
 * 只在按钮上给个禁用提示，真正的拒绝来自服务端。为什么：
 * 客户端的角色列表可能已经过期（别人刚改了你的角色），本地拦反而会
 * 挡住合法的操作；让服务端说了算，错了就提示错误。
 */
class MembersDialog : public QDialog {
  Q_OBJECT

 public:
  MembersDialog(Client* client, const QString& convId, const QString& selfId,
                QVariantList members, QWidget* parent = nullptr);

 private slots:
  void onInvite();
  void onKick();
  void onMute();
  void onUnmute();
  void onPromote();
  void onDemote();
  void refresh();

 private:
  [[nodiscard]] QString selectedUserId() const;
  [[nodiscard]] QString selectedRole() const;
  void callMemberOp(const QString& op, const QVariantMap& data, const QString& what);

  Client* client_ = nullptr;
  QString convId_;
  QString selfId_;
  QVariantList members_;

  QListWidget* list_ = nullptr;
  QPushButton* inviteButton_ = nullptr;
  QPushButton* kickButton_ = nullptr;
  QPushButton* muteButton_ = nullptr;
  QPushButton* unmuteButton_ = nullptr;
  QPushButton* promoteButton_ = nullptr;
  QPushButton* demoteButton_ = nullptr;
};

}  // namespace agentlink
