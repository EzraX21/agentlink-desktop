#include "main_window.hpp"

#include <QAction>
#include <QFileDialog>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>

#include "chat_window.hpp"
#include "conversation_list.hpp"
#include "core/auth_api.hpp"
#include "core/client.hpp"
#include "core/settings.hpp"
#include "onboarding.hpp"

namespace agentlink {

MainWindow::MainWindow(Client* client, AuthApi* api, SettingsStore* settings, QWidget* parent)
    : QMainWindow(parent), client_(client), api_(api), settings_(settings) {
  setWindowTitle(QStringLiteral("AgentLink"));
  resize(1000, 700);

  conversations_ = new ConversationList(client_, this);
  chat_ = new ChatWindow(client_, api_, this);

  splitter_ = new QSplitter(Qt::Horizontal, this);
  splitter_->addWidget(conversations_);
  splitter_->addWidget(chat_);
  splitter_->setStretchFactor(0, 1);
  splitter_->setStretchFactor(1, 3);
  setCentralWidget(splitter_);

  connect(conversations_, &ConversationList::selectionChanged, this,
          &MainWindow::onConversationSelected);
  connect(chat_, &ChatWindow::sendRequested, this, &MainWindow::onSendRequested);
  connect(chat_, &ChatWindow::attachmentRequested, this, &MainWindow::onAttachmentRequested);

  connect(client_, &Client::stateChanged, this, &MainWindow::onStateChanged);
  connect(client_, &Client::eventReceived, this, &MainWindow::onEvent);

  buildMenus();
}

/**
 * 菜单：让用户**随时**能加/换服务器。
 *
 * 这是"客户端在任意电脑上、自己手填服务器地址"的另一半 ——
 * 只在首次启动时问一次是不够的：用户可能要从公司服务器切到自己的，
 * 或者服务端换了地址。
 */
void MainWindow::buildMenus() {
  auto* serverMenu = menuBar()->addMenu(QStringLiteral("服务器"));

  auto* switchAction = serverMenu->addAction(QStringLiteral("添加 / 切换服务器…"));
  connect(switchAction, &QAction::triggered, this, &MainWindow::promptSwitchServer);

  auto* currentAction = serverMenu->addAction(QStringLiteral("当前服务器"));
  currentAction->setEnabled(false);
  currentAction->setText(
      QStringLiteral("当前服务器：%1").arg(client_->address().display().isEmpty()
                                               ? QStringLiteral("（未连接）")
                                               : client_->address().display()));

  serverMenu->addSeparator();
  auto* quitAction = serverMenu->addAction(QStringLiteral("退出"));
  connect(quitAction, &QAction::triggered, this, &QWidget::close);
}

void MainWindow::promptSwitchServer() {
  // 复用同一套引导流程：填地址 → 注册/登录。
  // 走完之后用新地址与新 token 重连（不重启进程）。
  Onboarding onboarding(api_, settings_, this);
  if (onboarding.exec() != QDialog::Accepted) return;

  const auto address = ServerAddress::parse(onboarding.serverAddress());
  if (!address.has_value()) {
    QMessageBox::warning(this, QStringLiteral("地址不对"),
                         QStringLiteral("这个地址解析不了：%1").arg(onboarding.serverAddress()));
    return;
  }

  api_->setHttpBase(address->httpBase());
  // 先清空界面：上一个服务器的会话列表不该留在屏幕上
  conversations_->setConversations({});
  chat_->openConversation(QString(), QString());
  chat_->setStatus(QStringLiteral("正在连接 %1 …").arg(address->display()));

  client_->connectTo(*address, onboarding.accessToken());
  emit serverChanged(address->url, onboarding.accessToken());
}

void MainWindow::onStateChanged(int state, const QString& detail) {
  const auto s = static_cast<ConnectionState>(state);
  switch (s) {
    case ConnectionState::Ready:
      statusBar()->showMessage(QStringLiteral("已连接"));
      refreshConversations();
      break;
    case ConnectionState::Connecting:
      statusBar()->showMessage(QStringLiteral("正在连接…"));
      break;
    case ConnectionState::Authenticating:
      statusBar()->showMessage(QStringLiteral("正在认证…"));
      break;
    case ConnectionState::Disconnected:
      statusBar()->showMessage(detail);
      break;
  }
  Q_UNUSED(detail)
}

void MainWindow::onEvent(const QString& type, const QVariantMap& data) {
  if (type == QStringLiteral("message.new")) {
    const QVariantMap message = data.value(QStringLiteral("message")).toMap();
    chat_->appendMessage(message);
    // 顺手更新会话列表的顺序（最近活跃的排前面）—— 简化版：有新消息就刷新一次。
    QTimer::singleShot(0, this, &MainWindow::refreshConversations);
    return;
  }
  if (type == QStringLiteral("message.edit")) {
    const QVariantMap message = data.value(QStringLiteral("message")).toMap();
    // 简单起见：重开当前会话重新渲染（v1 的编辑频率低，不值得做增量）。
    if (!chat_->conversationId().isEmpty()) {
      chat_->openConversation(chat_->conversationId(), convTitle_);
    }
    return;
  }
  if (type == QStringLiteral("message.revoke")) {
    if (!chat_->conversationId().isEmpty()) {
      chat_->openConversation(chat_->conversationId(), convTitle_);
    }
    return;
  }
  if (type == QStringLiteral("conv.update") || type == QStringLiteral("group.update")) {
    QTimer::singleShot(0, this, &MainWindow::refreshConversations);
    return;
  }
}

void MainWindow::refreshConversations() {
  if (client_ == nullptr) return;
  client_->send(QStringLiteral("conv.list"), QVariantMap(),
                [this](bool ok, const QVariantMap& data, const WireError& error) {
                  if (!ok) {
                    chat_->setStatus(QStringLiteral("取会话列表失败：%1").arg(error.message), true);
                    return;
                  }
                  conversations_->setConversations(data.value(QStringLiteral("items")).toList());
                });
}

void MainWindow::onConversationSelected(const QString& convId) {
  if (convId.isEmpty()) return;
  // 标题从会话列表里拿
  client_->send(QStringLiteral("conv.get"), QVariantMap{{QStringLiteral("conv_id"), convId}},
                [this, convId](bool ok, const QVariantMap& data, const WireError&) {
                  if (!ok) return;
                  const QVariantMap conv = data.value(QStringLiteral("conv")).toMap();
                  convTitle_ = conv.value(QStringLiteral("title")).toString();
                  chat_->openConversation(convId, convTitle_);
                });
}

void MainWindow::onSendRequested(const QString& text) {
  sendMessage(text);
}

void MainWindow::sendMessage(const QString& text) {
  const QString convId = chat_->conversationId();
  if (convId.isEmpty()) {
    chat_->setStatus(QStringLiteral("先选一个会话"), true);
    return;
  }

  // 乐观回显：先显示，服务端回推同一条时再对齐。
  QVariantMap echo;
  echo.insert(QStringLiteral("conv_id"), convId);
  echo.insert(QStringLiteral("sender"), QVariantMap{{QStringLiteral("id"), client_->selfId()},
                                                     {QStringLiteral("display_name"),
                                                      QStringLiteral("我")}});
  echo.insert(QStringLiteral("content"),
              QVariantList{QVariantMap{{QStringLiteral("type"), QStringLiteral("text")},
                                      {QStringLiteral("text"), text}}});
  echo.insert(QStringLiteral("created_at"),
              static_cast<double>(QDateTime::currentMSecsSinceEpoch()));
  echo.insert(QStringLiteral("revoked_at"), QVariant());
  echo.insert(QStringLiteral("edited_at"), QVariant());
  chat_->appendLocalEcho(echo);

  QVariantMap content;
  content.insert(QStringLiteral("type"), QStringLiteral("text"));
  content.insert(QStringLiteral("text"), text);

  const QString clientMsgId =
      QStringLiteral("qt-%1").arg(QDateTime::currentMSecsSinceEpoch());

  client_->send(QStringLiteral("message.send"),
                QVariantMap{{QStringLiteral("conv_id"), convId},
                            {QStringLiteral("client_msg_id"), clientMsgId},
                            {QStringLiteral("content"), QVariantList{content}}},
                [this](bool ok, const QVariantMap&, const WireError& error) {
                  if (!ok) {
                    chat_->setStatus(QStringLiteral("发送失败：%1").arg(error.message), true);
                  }
                });
}

void MainWindow::onAttachmentRequested() {
  const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择要发送的文件"));
  if (path.isEmpty()) return;
  sendAttachment(path);
}

void MainWindow::sendAttachment(const QString& filePath) {
  const QString convId = chat_->conversationId();
  if (convId.isEmpty()) {
    chat_->setStatus(QStringLiteral("先选一个会话"), true);
    return;
  }

  chat_->setStatus(QStringLiteral("正在上传…"));
  const ServerEntry* entry = settings_->activeEntry();
  const QString token = entry == nullptr ? QString() : entry->accessToken;

  api_->uploadAttachment(token, filePath, [this, convId](bool ok, const QVariantMap& info,
                                                        const QString& error) {
    if (!ok) {
      chat_->setStatus(QStringLiteral("上传失败：%1").arg(error), true);
      return;
    }

    QVariantMap block;
    block.insert(QStringLiteral("type"), QStringLiteral("file"));
    block.insert(QStringLiteral("attachment_id"), info.value(QStringLiteral("attachment_id")));
    block.insert(QStringLiteral("filename"), info.value(QStringLiteral("filename")));
    block.insert(QStringLiteral("size"), info.value(QStringLiteral("size")));

    client_->send(QStringLiteral("message.send"),
                  QVariantMap{{QStringLiteral("conv_id"), convId},
                              {QStringLiteral("client_msg_id"),
                               QStringLiteral("qt-att-%1").arg(QDateTime::currentMSecsSinceEpoch())},
                              {QStringLiteral("content"), QVariantList{block}}},
                  [this](bool sent, const QVariantMap&, const WireError& err) {
                    if (!sent) {
                      chat_->setStatus(QStringLiteral("发送失败：%1").arg(err.message), true);
                    } else {
                      chat_->setStatus(QString());
                    }
                  });
  });
}

void MainWindow::requireLogin() {
  chat_->setStatus(QStringLiteral("登录已失效，请重新登录"), true);
}

}  // namespace agentlink
