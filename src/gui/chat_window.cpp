#include "chat_window.hpp"

#include <QDateTime>
#include <QHBoxLayout>
#include <QScrollBar>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

#include "core/auth_api.hpp"
#include "core/client.hpp"

namespace agentlink {
namespace {

/// 时间戳 → "14:32"（今天）或 "11-18 14:32"（别的日子）。
QString formatTime(qint64 ms) {
  const QDateTime dt = QDateTime::fromMSecsSinceEpoch(ms);
  const QDateTime now = QDateTime::currentDateTime();
  if (dt.date() == now.date()) {
    return dt.toString(QStringLiteral("HH:mm"));
  }
  return dt.toString(QStringLiteral("MM-dd HH:mm"));
}

/// 把用户输入里的危险字符转义掉。
///
/// 我们**不**支持富文本输入（没有加粗按钮），所以输入框里的内容一律当纯文本。
/// 但引用的 `appendHtml` 仍会解析 HTML —— 所以必须转义，
/// 否则用户发 `<b>hi</b>` 会变成真的加粗，发 `<script>` 更糟。
QString escapeHtml(const QString& s) {
  QString out = s;
  out.replace(QStringLiteral("&"), QStringLiteral("&amp;"));
  out.replace(QStringLiteral("<"), QStringLiteral("&lt;"));
  out.replace(QStringLiteral(">"), QStringLiteral("&gt;"));
  return out;
}

}  // namespace

ChatWindow::ChatWindow(Client* client, AuthApi* api, QWidget* parent)
    : QWidget(parent), client_(client), api_(api) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  statusLabel_ = new QLabel(this);
  statusLabel_->setObjectName(QStringLiteral("statusLabel"));
  statusLabel_->setVisible(false);
  layout->addWidget(statusLabel_);

  browser_ = new QTextBrowser(this);
  browser_->setObjectName(QStringLiteral("messageBrowser"));
  browser_->setOpenExternalLinks(false);
  layout->addWidget(browser_, 1);

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
  browser_->clear();

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
                  browser_->verticalScrollBar()->setValue(browser_->verticalScrollBar()->maximum());
                });
}

QString ChatWindow::renderContent(const QVariantList& content) const {
  QString html;
  for (const QVariant& v : content) {
    const QVariantMap block = v.toMap();
    const QString type = block.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("text")) {
      html += escapeHtml(block.value(QStringLiteral("text")).toString());
    } else if (type == QStringLiteral("image")) {
      const QString name = block.value(QStringLiteral("filename")).toString();
      html += QStringLiteral("<span style=\"color:#888\">🖼 %1</span>").arg(escapeHtml(name));
    } else {
      const QString name = block.value(QStringLiteral("filename")).toString();
      html += QStringLiteral("<span style=\"color:#888\">📎 %1</span>").arg(escapeHtml(name));
    }
    html += QStringLiteral("<br>");
  }
  return html;
}

void ChatWindow::renderMessage(const QVariantMap& message, bool isSelf) {
  const QVariantMap sender = message.value(QStringLiteral("sender")).toMap();
  const QString senderId = sender.value(QStringLiteral("id")).toString();
  const QString senderName = sender.value(QStringLiteral("display_name")).toString();
  const QString content = renderContent(message.value(QStringLiteral("content")).toList());
  const qint64 createdAt =
      static_cast<qint64>(message.value(QStringLiteral("created_at")).toDouble());
  const bool revoked = !message.value(QStringLiteral("revoked_at")).isNull();
  const bool edited = !message.value(QStringLiteral("edited_at")).isNull();

  QString name = senderName;
  if (isSelf || selfId_.isEmpty()) {
    name = senderName.isEmpty() ? QStringLiteral("我") : senderName;
  }

  QString html = QStringLiteral("<div style=\"margin:6px 0;\">");
  html += QStringLiteral("<span style=\"color:#888;font-size:11px;\">%1 · %2</span> ")
             .arg(escapeHtml(name), formatTime(createdAt));
  if (revoked) {
    html += QStringLiteral("<span style=\"color:#c0392b;font-size:11px;\">（已撤回）</span>");
  } else if (edited) {
    html += QStringLiteral("<span style=\"color:#888;font-size:11px;\">（已编辑）</span>");
  }
  html += QStringLiteral("<br>");
  html += revoked ? QStringLiteral("<i style=\"color:#999\">这条消息已被撤回</i>") : content;
  html += QStringLiteral("</div>");

  browser_->append(html);
}

void ChatWindow::appendMessage(const QVariantMap& message) {
  if (message.value(QStringLiteral("conv_id")).toString() != convId_) return;
  const QVariantMap sender = message.value(QStringLiteral("sender")).toMap();
  const bool isSelf = sender.value(QStringLiteral("id")).toString() == selfId_;
  renderMessage(message, isSelf);
  browser_->verticalScrollBar()->setValue(browser_->verticalScrollBar()->maximum());
}

void ChatWindow::appendLocalEcho(const QVariantMap& message) {
  renderMessage(message, true);
  browser_->verticalScrollBar()->setValue(browser_->verticalScrollBar()->maximum());
}

void ChatWindow::onSendClicked() {
  const QString text = input_->text().trimmed();
  if (text.isEmpty()) return;
  input_->clear();
  emit sendRequested(text);
}

void ChatWindow::onReturnPressed() {
  onSendClicked();
}

void ChatWindow::onAttachClicked() {
  emit attachmentRequested();
}

}  // namespace agentlink
