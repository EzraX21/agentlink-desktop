#include "chat_window.hpp"

#include <QAbstractItemView>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>

#include "core/auth_api.hpp"
#include "core/client.hpp"
#include "message_item.hpp"

namespace agentlink {
namespace {

/// 把消息正文里所有 text 块拼成一个纯文本（quote 预览 / 复制用）。
QString messageText(const QVariantMap& message) {
  QString text;
  const QVariantList blocks = message.value(QStringLiteral("content")).toList();
  for (const QVariant& b : blocks) {
    const QVariantMap block = b.toMap();
    if (block.value(QStringLiteral("type")).toString() == QStringLiteral("text")) {
      text += block.value(QStringLiteral("text")).toString();
    }
  }
  return text;
}

}  // namespace

ChatWindow::ChatWindow(Client* client, AuthApi* api, std::function<QString()> token, QWidget* parent)
    : QWidget(parent), client_(client), api_(api), token_(std::move(token)) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  statusLabel_ = new QLabel(this);
  statusLabel_->setObjectName(QStringLiteral("statusLabel"));
  statusLabel_->setVisible(false);
  layout->addWidget(statusLabel_);

  list_ = new QListWidget(this);
  list_->setObjectName(QStringLiteral("messageList"));
  list_->setSelectionMode(QAbstractItemView::NoSelection);
  list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  layout->addWidget(list_, 1);

  // 回复条：非空时显示"回复 xxx"，带取消按钮
  auto* replyRow = new QWidget(this);
  auto* replyLayout = new QHBoxLayout(replyRow);
  replyLayout->setContentsMargins(8, 2, 8, 2);
  replyBar_ = new QLabel(replyRow);
  replyBar_->setObjectName(QStringLiteral("replyBar"));
  replyBar_->setWordWrap(true);
  replyBar_->setVisible(false);
  replyCancelButton_ = new QPushButton(QStringLiteral("✕"), replyRow);
  replyCancelButton_->setFixedWidth(28);
  replyCancelButton_->setVisible(false);
  replyLayout->addWidget(replyBar_, 1);
  replyLayout->addWidget(replyCancelButton_);
  layout->addWidget(replyRow);
  connect(replyCancelButton_, &QPushButton::clicked, this, &ChatWindow::cancelReply);

  auto* inputRow = new QWidget(this);
  auto* inputLayout = new QHBoxLayout(inputRow);
  inputLayout->setContentsMargins(8, 4, 8, 8);

  attachButton_ = new QPushButton(QStringLiteral("📎"), inputRow);
  attachButton_->setToolTip(QStringLiteral("发送文件"));
  attachButton_->setFixedWidth(36);

  input_ = new QLineEdit(inputRow);
  input_->setObjectName(QStringLiteral("messageInput"));
  input_->setPlaceholderText(QStringLiteral("输入消息…"));

  sendButton_ = new QPushButton(QStringLiteral("发送"), inputRow);

  inputLayout->addWidget(attachButton_);
  inputLayout->addWidget(input_, 1);
  inputLayout->addWidget(sendButton_);
  layout->addWidget(inputRow);

  connect(sendButton_, &QPushButton::clicked, this, &ChatWindow::onSendClicked);
  connect(attachButton_, &QPushButton::clicked, this, &ChatWindow::onAttachClicked);
  connect(input_, &QLineEdit::returnPressed, this, &ChatWindow::onReturnPressed);
  connect(input_, &QLineEdit::textChanged, this, &ChatWindow::typing);
}

void ChatWindow::setStatus(const QString& text, bool isError) {
  statusLabel_->setText(text);
  statusLabel_->setVisible(!text.isEmpty());
  statusLabel_->setStyleSheet(
      isError ? QStringLiteral("color: #c0392b;") : QStringLiteral("color: #7f8c8d;"));
}

void ChatWindow::openConversation(const QString& convId, const QString& title) {
  convId_ = convId;
  convTitle_ = title;
  list_->clear();
  streamAcc_.clear();
  textById_.clear();
  clearReplyTo();

  if (client_ == nullptr || convId.isEmpty()) return;

  client_->send(QStringLiteral("history.fetch"),
                QVariantMap{{QStringLiteral("conv_id"), convId}, {QStringLiteral("limit"), 50}},
                [this](bool ok, const QVariantMap& data, const WireError& error) {
                  if (!ok) {
                    setStatus(QStringLiteral("读历史失败：%1").arg(error.message), true);
                    return;
                  }
                  // 服务端返回新的在前，倒过来渲染才是聊天顺序。
                  const QVariantList messages = data.value(QStringLiteral("messages")).toList();
                  for (int i = messages.size() - 1; i >= 0; --i) {
                    renderMessage(messages.at(i).toMap(), false);
                  }
                  scrollToBottom();
                });
}

void ChatWindow::renderMessage(const QVariantMap& message, bool isSelf) {
  const QString messageId = message.value(QStringLiteral("id")).toString();
  if (messageId.isEmpty()) return;

  const QVariantMap sender = message.value(QStringLiteral("sender")).toMap();
  const bool self = sender.value(QStringLiteral("id")).toString() == selfId_;
  const bool effectiveSelf = isSelf || (self && !selfId_.isEmpty());

  auto* item = new MessageItem(api_, token_, this);
  item->setQuoteResolver([this](const QString& id) { return resolveQuote(id); });
  item->setSelfId(selfId_);
  item->setCanPin(canPin_);
  item->setMessage(message, effectiveSelf);

  connect(item, &MessageItem::replyRequested, this, &ChatWindow::setReplyTo);
  connect(item, &MessageItem::revokeRequested, this, &ChatWindow::revokeRequested);
  connect(item, &MessageItem::editRequested, this, &ChatWindow::editRequested);
  connect(item, &MessageItem::forwardRequested, this, &ChatWindow::forwardRequested);
  connect(item, &MessageItem::pinRequested, this, &ChatWindow::pinRequested);
  connect(item, &MessageItem::reactionRequested, this, &ChatWindow::reactionRequested);
  connect(item, &MessageItem::interruptRequested, this, &ChatWindow::interruptRequested);
  connect(item, &MessageItem::imageClicked, this, &ChatWindow::imageRequested);
  connect(item, &MessageItem::fileClicked, this, &ChatWindow::fileRequested);

  auto* listItem = new QListWidgetItem(list_);
  listItem->setSizeHint(item->sizeHint());
  list_->setItemWidget(listItem, item);

  // 记住正文，供 quote 解析
  const QString text = messageText(message);
  if (!text.isEmpty()) textById_.insert(messageId, text);
}

void ChatWindow::appendMessage(const QVariantMap& message) {
  if (message.value(QStringLiteral("conv_id")).toString() != convId_) return;
  renderMessage(message, false);
  scrollToBottom();
}

void ChatWindow::appendLocalEcho(const QVariantMap& message) {
  renderMessage(message, true);
  scrollToBottom();
}

void ChatWindow::applyStreamDelta(const QString& convId, const QString& messageId, const QString& delta) {
  if (convId != convId_) return;
  streamAcc_[messageId] += delta;
  // 找到对应 item 更新流式正文
  for (int i = 0; i < list_->count(); ++i) {
    auto* item = qobject_cast<MessageItem*>(list_->itemWidget(list_->item(i)));
    if (item != nullptr && item->messageId() == messageId) {
      item->updateStreaming(streamAcc_.value(messageId), true);
      scrollToBottom();
      return;
    }
  }
}

void ChatWindow::applyMessageFinal(const QVariantMap& message) {
  if (message.value(QStringLiteral("conv_id")).toString() != convId_) return;
  const QString messageId = message.value(QStringLiteral("id")).toString();
  streamAcc_.remove(messageId);
  renderMessage(message, false);
  scrollToBottom();
}

void ChatWindow::applyMessageRevoke(const QString& convId, const QString& messageId) {
  if (convId != convId_) return;
  // 撤回：本地标记"已撤回"，不要重拉历史 —— 服务端 history.fetch 会过滤掉
  // 已撤回的消息，重拉会让这条消息凭空消失而不是显示"已撤回"。
  for (int i = 0; i < list_->count(); ++i) {
    auto* item = qobject_cast<MessageItem*>(list_->itemWidget(list_->item(i)));
    if (item != nullptr && item->messageId() == messageId) {
      item->markRevoked();
      return;
    }
  }
}

void ChatWindow::applyMessageEdit(const QVariantMap& message) {
  if (message.value(QStringLiteral("conv_id")).toString() != convId_) return;
  const QString messageId = message.value(QStringLiteral("id")).toString();
  // 原地替换：找到 item 重渲染
  for (int i = 0; i < list_->count(); ++i) {
    auto* item = qobject_cast<MessageItem*>(list_->itemWidget(list_->item(i)));
    if (item != nullptr && item->messageId() == messageId) {
      const QVariantMap sender = message.value(QStringLiteral("sender")).toMap();
      const bool self = sender.value(QStringLiteral("id")).toString() == selfId_;
      item->setMessage(message, self && !selfId_.isEmpty());
      const QString text = messageText(message);
      if (!text.isEmpty()) textById_.insert(messageId, text);
      return;
    }
  }
}

void ChatWindow::applyReaction(const QString& convId, const QString& messageId,
                               const QVariantList& reactions) {
  if (convId != convId_) return;
  for (int i = 0; i < list_->count(); ++i) {
    auto* item = qobject_cast<MessageItem*>(list_->itemWidget(list_->item(i)));
    if (item != nullptr && item->messageId() == messageId) {
      item->setReactions(reactions);
      return;
    }
  }
}

void ChatWindow::showTyping(const QString& convId, const QString& userId, const QString& displayName) {
  if (convId != convId_ || userId == selfId_) return;
  const QString name = displayName.isEmpty() ? QStringLiteral("有人") : displayName;
  setStatus(QStringLiteral("%1 正在输入…").arg(name), false);
  // 3 秒后自动清（服务端 TYPING_TTL_MS = 3000，客户端也按这个值过期）
  QTimer::singleShot(3000, this, [this]() {
    statusLabel_->setVisible(false);
  });
}

void ChatWindow::setReplyTo(const QString& messageId) {
  replyToId_ = messageId;
  const QString quoted = resolveQuote(messageId);
  replyBar_->setText(QStringLiteral("回复：%1").arg(quoted.isEmpty() ? QStringLiteral("…") : quoted));
  replyBar_->setVisible(true);
  replyCancelButton_->setVisible(true);
  input_->setFocus();
}

void ChatWindow::clearReplyTo() {
  replyToId_.clear();
  replyBar_->setVisible(false);
  replyCancelButton_->setVisible(false);
}

void ChatWindow::cancelReply() {
  clearReplyTo();
}

QString ChatWindow::resolveQuote(const QString& messageId) const {
  return textById_.value(messageId, QString());
}

void ChatWindow::scrollToBottom() {
  list_->verticalScrollBar()->setValue(list_->verticalScrollBar()->maximum());
}

void ChatWindow::onSendClicked() {
  const QString text = input_->text().trimmed();
  if (text.isEmpty()) return;
  input_->clear();
  const QString replyTo = replyToId_;
  clearReplyTo();
  emit sendRequested(text, replyTo);
}

void ChatWindow::onReturnPressed() {
  onSendClicked();
}

void ChatWindow::onAttachClicked() {
  emit attachmentRequested();
}

}  // namespace agentlink
