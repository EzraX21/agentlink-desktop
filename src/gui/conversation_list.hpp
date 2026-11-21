#pragma once

#include <QWidget>
#include <QVariantMap>
#include <functional>
#include <vector>

class QListWidget;
class QListWidgetItem;

namespace agentlink {

class Client;
class AuthApi;

/**
 * 会话列表（左侧栏）。
 *
 * 只做两件事：显示会话、告诉上层"用户选了哪个"。
 * 消息逻辑全在 ChatWindow 里 —— 别把两个搅在一起。
 */
class ConversationList : public QWidget {
  Q_OBJECT

 public:
  ConversationList(Client* client, QWidget* parent = nullptr);

  /// 收到会话列表后刷新。
  void setConversations(const QVariantList& conversations);

  /// 选中的会话 id（没选则空）。
  [[nodiscard]] QString selectedConversationId() const;

  /// 程序化选中某个会话（转发、通知跳转时用）。
  void selectConversation(const QString& convId);

  /// 用户选了会话。
  using SelectionHandler = std::function<void(const QString& convId)>;
  void setSelectionHandler(SelectionHandler handler) { selectionHandler_ = std::move(handler); }

  /// 未读角标（协议：客户端自己数，v1 服务端不下发未读数）。
  void setUnread(const QString& convId, int count);

 signals:
  /// 用户选了会话（connect 用，见 main_window.cpp）。
  void selectionChanged(const QString& convId);

 private:
  QListWidget* list_ = nullptr;
  QListWidgetItem* itemFor(const QString& convId) const;

  Client* client_ = nullptr;
  SelectionHandler selectionHandler_;
};

}  // namespace agentlink
