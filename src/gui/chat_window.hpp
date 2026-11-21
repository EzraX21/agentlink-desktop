#pragma once

#include <QHash>
#include <QWidget>
#include <QVariantMap>
#include <functional>

class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;

namespace agentlink {

class Client;
class AuthApi;
class MessageItem;

/**
 * 聊天窗口（右侧栏）：消息流 + 输入框。
 *
 * 消息流用 QListWidget + 每条消息一个 MessageItem（见 message_item.hpp），
 * 这样单条消息的右键、流式更新、内联图片都是"改一个 widget"，
 * 而不是在 QTextBrowser 里做字符串手术。
 *
 * 流式：服务端按 `message.new`（空 streaming）→ `message.update`（delta，seq=null）
 * → `message.final`（完整消息）的顺序推。这里维护一个 delta 累积表，
 * 到 final 时用完整消息整体替换。
 */
class ChatWindow : public QWidget {
  Q_OBJECT

 public:
  ChatWindow(Client* client, AuthApi* api, std::function<QString()> token, QWidget* parent = nullptr);

  /// 切到某个会话：清空并拉历史。
  void openConversation(const QString& convId, const QString& title);

  [[nodiscard]] QString conversationId() const { return convId_; }

  /// 自己的 id（用来判断"这条是不是我发的"）。
  void setSelfId(const QString& id) { selfId_ = id; }

  /// 收到 message.new：插入一条。
  void appendMessage(const QVariantMap& message);

  /// 本地发出去的回显（乐观插入）。
  void appendLocalEcho(const QVariantMap& message);

  /// 收到 message.update：把 delta 累加到流式消息上。
  void applyStreamDelta(const QString& convId, const QString& messageId, const QString& delta);

  /// 收到 message.final：用完整消息替换流式占位。
  void applyMessageFinal(const QVariantMap& message);

  /// 收到 message.revoke：把对应消息标成已撤回。
  void applyMessageRevoke(const QString& convId, const QString& messageId);

  /// 收到 message.edit：原地替换正文。
  void applyMessageEdit(const QVariantMap& message);

  /// 设置连接状态提示。
  void setStatus(const QString& text, bool isError = false);

  /// 需要重新登录（token 失效）。
  using AuthRequiredHandler = std::function<void()>;
  void setAuthRequiredHandler(AuthRequiredHandler handler) { authRequired_ = std::move(handler); }

 signals:
  /// 用户点了发送。replyToId 非空表示这是一条回复。
  void sendRequested(const QString& text, const QString& replyToId);
  /// 用户要发附件。
  void attachmentRequested();
  /// 右键撤回。
  void revokeRequested(const QString& messageId);
  /// 打断 AI 流式。
  void interruptRequested(const QString& messageId);
  /// 点开图片看大图。
  void imageRequested(const QString& attachmentId);
  /// 点开文件。
  void fileRequested(const QString& attachmentId, const QString& filename);

 private slots:
  void onSendClicked();
  void onReturnPressed();
  void onAttachClicked();
  void cancelReply();

 private:
  void renderMessage(const QVariantMap& message, bool isSelf);
  void setReplyTo(const QString& messageId);
  void clearReplyTo();
  QString resolveQuote(const QString& messageId) const;
  void scrollToBottom();

  Client* client_ = nullptr;
  AuthApi* api_ = nullptr;
  std::function<QString()> token_;

  QLabel* statusLabel_ = nullptr;
  QLabel* replyBar_ = nullptr;
  QPushButton* replyCancelButton_ = nullptr;
  QListWidget* list_ = nullptr;
  QLineEdit* input_ = nullptr;
  QPushButton* sendButton_ = nullptr;
  QPushButton* attachButton_ = nullptr;

  QString convId_;
  QString convTitle_;
  QString selfId_;
  QString replyToId_;

  /// message id → 流式累积文本（message.update 是增量，到 final 才完整）。
  QHash<QString, QString> streamAcc_;
  /// message id → 正文（quote 解析用）。
  QHash<QString, QString> textById_;

  AuthRequiredHandler authRequired_;
};

}  // namespace agentlink
