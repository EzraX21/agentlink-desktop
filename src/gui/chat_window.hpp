#pragma once

#include <QWidget>
#include <QVariantMap>
#include <functional>
#include <vector>

class QTextBrowser;
class QLineEdit;
class QPushButton;
class QLabel;

namespace agentlink {

class Client;
class AuthApi;

/**
 * 聊天窗口（右侧栏）：消息流 + 输入框。
 *
 * ## 为什么用 QTextBrowser 而不是 QListWidget + delegate
 *
 * 消息气泡是**变高**的富文本（引用、附件、多行文本）。
 * QTextBrowser 自带文档流与富文本排版，比自己算 item 高度省事得多，
 * 而且天然支持"滚到底部"。
 *
 * 代价是选中/复制某个气泡里的文本要靠 QTextCursor —— 对 v1 够用。
 */
class ChatWindow : public QWidget {
  Q_OBJECT

 public:
  ChatWindow(Client* client, AuthApi* api, QWidget* parent = nullptr);

  /// 切到某个会话：清空并拉历史。
  void openConversation(const QString& convId, const QString& title);

  /// 当前会话 id。
  [[nodiscard]] QString conversationId() const { return convId_; }

  /// 收到一条新消息（来自事件）。
  void appendMessage(const QVariantMap& message);

  /// 本地发出去的回显（乐观插入）。
  void appendLocalEcho(const QVariantMap& message);

  /// 设置连接状态提示。
  void setStatus(const QString& text, bool isError = false);

  /// 需要重新登录（token 失效）。
  using AuthRequiredHandler = std::function<void()>;
  void setAuthRequiredHandler(AuthRequiredHandler handler) { authRequired_ = std::move(handler); }

 signals:
  /// 用户点了发送。
  void sendRequested(const QString& text);
  /// 用户要发附件。
  void attachmentRequested();

 private slots:
  void onSendClicked();
  void onReturnPressed();
  void onAttachClicked();

 private:
  void renderMessage(const QVariantMap& message, bool isSelf);
  QString renderContent(const QVariantList& content) const;

  Client* client_ = nullptr;
  AuthApi* api_ = nullptr;

  QTextBrowser* browser_ = nullptr;
  QLineEdit* input_ = nullptr;
  QPushButton* sendButton_ = nullptr;
  QPushButton* attachButton_ = nullptr;
  QLabel* statusLabel_ = nullptr;

  QString convId_;
  QString convTitle_;
  QString selfId_;

  AuthRequiredHandler authRequired_;
};

}  // namespace agentlink
