#include "main_window.hpp"

#include <QAction>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QFile>
#include <QFileDialog>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPixmap>
#include <QScrollArea>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "chat_window.hpp"
#include "conversation_list.hpp"
#include "core/auth_api.hpp"
#include "core/client.hpp"
#include "core/settings.hpp"
#include "create_group_dialog.hpp"
#include "group_info_dialog.hpp"
#include "members_dialog.hpp"
#include "onboarding.hpp"
#include "search_panel.hpp"

namespace agentlink {

MainWindow::MainWindow(Client* client, AuthApi* api, SettingsStore* settings, QWidget* parent)
    : QMainWindow(parent), client_(client), api_(api), settings_(settings) {
  setWindowTitle(QStringLiteral("AgentLink"));
  resize(1000, 700);

  conversations_ = new ConversationList(client_, this);
  chat_ = new ChatWindow(
      client_, api_,
      // token 取值器：上传/下载/缩略图都要当前登录态。用 lambda 保证永远读最新值。
      [this]() -> QString {
        const ServerEntry* entry = settings_->activeEntry();
        return entry == nullptr ? QString() : entry->accessToken;
      },
      this);
  searchPanel_ = new SearchPanel(client_, this);
  searchPanel_->hide();

  splitter_ = new QSplitter(Qt::Horizontal, this);
  splitter_->addWidget(searchPanel_);
  splitter_->addWidget(conversations_);
  splitter_->addWidget(chat_);
  splitter_->setStretchFactor(1, 1);
  splitter_->setStretchFactor(2, 3);
  setCentralWidget(splitter_);

  connect(conversations_, &ConversationList::selectionChanged, this,
          &MainWindow::onConversationSelected);
  connect(chat_, &ChatWindow::sendRequested, this, &MainWindow::onSendRequested);
  connect(chat_, &ChatWindow::attachmentRequested, this, &MainWindow::onAttachmentRequested);
  connect(chat_, &ChatWindow::revokeRequested, this, &MainWindow::onRevokeRequested);
  connect(chat_, &ChatWindow::editRequested, this, &MainWindow::onEditRequested);
  connect(chat_, &ChatWindow::forwardRequested, this, &MainWindow::onForwardRequested);
  connect(chat_, &ChatWindow::pinRequested, this, &MainWindow::onPinRequested);
  connect(chat_, &ChatWindow::reactionRequested, this, &MainWindow::onReactionRequested);
  connect(chat_, &ChatWindow::interruptRequested, this, &MainWindow::onInterruptRequested);
  connect(chat_, &ChatWindow::imageRequested, this, &MainWindow::onImageRequested);
  connect(chat_, &ChatWindow::fileRequested, this, &MainWindow::onFileRequested);
  connect(chat_, &ChatWindow::typing, this, &MainWindow::onInputTextChanged);
  connect(searchPanel_, &SearchPanel::resultClicked, this, &MainWindow::onJumpToMessage);

  connect(client_, &Client::stateChanged, this, &MainWindow::onStateChanged);
  connect(client_, &Client::eventReceived, this, &MainWindow::onEvent);

  buildMenus();
}

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

  auto* conversationMenu = menuBar()->addMenu(QStringLiteral("会话"));

  auto* newGroupAction = conversationMenu->addAction(QStringLiteral("建群…"));
  connect(newGroupAction, &QAction::triggered, this, &MainWindow::onCreateGroup);

  auto* groupInfoAction = conversationMenu->addAction(QStringLiteral("群信息…"));
  connect(groupInfoAction, &QAction::triggered, this, &MainWindow::onOpenGroupInfo);

  auto* membersAction = conversationMenu->addAction(QStringLiteral("成员管理…"));
  connect(membersAction, &QAction::triggered, this, &MainWindow::onOpenMembers);

  auto* searchAction = conversationMenu->addAction(QStringLiteral("搜索消息…"));
  searchAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+K")));
  connect(searchAction, &QAction::triggered, this, &MainWindow::onToggleSearch);

  serverMenu->addSeparator();
  auto* quitAction = serverMenu->addAction(QStringLiteral("退出"));
  connect(quitAction, &QAction::triggered, this, &QWidget::close);
}

void MainWindow::promptSwitchServer() {
  Onboarding onboarding(api_, settings_, this);
  if (onboarding.exec() != QDialog::Accepted) return;

  const auto address = ServerAddress::parse(onboarding.serverAddress());
  if (!address.has_value()) {
    QMessageBox::warning(this, QStringLiteral("地址不对"),
                         QStringLiteral("这个地址解析不了：%1").arg(onboarding.serverAddress()));
    return;
  }

  api_->setHttpBase(address->httpBase());
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
      chat_->setSelfId(client_->selfId());
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
    // 正在看这个会话 → 顺手标记已读（读的是消息自己的 seq）
    if (message.value(QStringLiteral("conv_id")).toString() == chat_->conversationId()) {
      const qint64 seq = static_cast<qint64>(message.value(QStringLiteral("seq")).toDouble());
      if (seq > 0) reportRead(seq);
    }
    QTimer::singleShot(0, this, &MainWindow::refreshConversations);
    return;
  }
  if (type == QStringLiteral("message.update")) {
    chat_->applyStreamDelta(data.value(QStringLiteral("conv_id")).toString(),
                            data.value(QStringLiteral("message_id")).toString(),
                            data.value(QStringLiteral("delta")).toString());
    return;
  }
  if (type == QStringLiteral("message.final")) {
    const QVariantMap message = data.value(QStringLiteral("message")).toMap();
    chat_->applyMessageFinal(message);
    if (message.value(QStringLiteral("conv_id")).toString() == chat_->conversationId()) {
      const qint64 seq = static_cast<qint64>(message.value(QStringLiteral("seq")).toDouble());
      if (seq > 0) reportRead(seq);
    }
    return;
  }
  if (type == QStringLiteral("message.revoke")) {
    chat_->applyMessageRevoke(data.value(QStringLiteral("conv_id")).toString(),
                              data.value(QStringLiteral("message_id")).toString());
    return;
  }
  if (type == QStringLiteral("message.edit")) {
    chat_->applyMessageEdit(data.value(QStringLiteral("message")).toMap());
    return;
  }
  if (type == QStringLiteral("message.reaction")) {
    chat_->applyReaction(data.value(QStringLiteral("conv_id")).toString(),
                         data.value(QStringLiteral("message_id")).toString(),
                         data.value(QStringLiteral("reactions")).toList());
    return;
  }
  if (type == QStringLiteral("typing")) {
    chat_->showTyping(data.value(QStringLiteral("conv_id")).toString(),
                      data.value(QStringLiteral("user_id")).toString(),
                      data.value(QStringLiteral("display_name")).toString());
    return;
  }
  if (type == QStringLiteral("message.pin")) {
    // 置顶变了：重拉会话列表（pinned_message_id 在会话对象上）。
    QTimer::singleShot(0, this, &MainWindow::refreshConversations);
    if (chat_->conversationId() == data.value(QStringLiteral("conv_id")).toString()) {
      chat_->setStatus(QStringLiteral("置顶已更新"));
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
  client_->send(QStringLiteral("conv.get"), QVariantMap{{QStringLiteral("conv_id"), convId}},
                [this, convId](bool ok, const QVariantMap& data, const WireError&) {
                  if (!ok) return;
                  const QVariantMap conv = data.value(QStringLiteral("conv")).toMap();
                  convTitle_ = conv.value(QStringLiteral("title")).toString();
                  convAnnouncement_ = conv.value(QStringLiteral("announcement")).toString();
                  members_ = data.value(QStringLiteral("members")).toList();

                  // 我的角色决定能不能置顶 / 编辑群信息
                  const QString selfId = client_->selfId();
                  bool isAdmin = false;
                  for (const QVariant& m : members_) {
                    const QVariantMap member = m.toMap();
                    if (member.value(QStringLiteral("user_id")).toString() == selfId) {
                      const QString role = member.value(QStringLiteral("role")).toString();
                      isAdmin = role == QStringLiteral("owner") || role == QStringLiteral("admin");
                    }
                  }
                  chat_->setCanPin(isAdmin);
                  chat_->openConversation(convId, convTitle_);
                });
}

void MainWindow::onSendRequested(const QString& text, const QString& replyToId) {
  sendMessage(text, replyToId);
}

void MainWindow::sendMessage(const QString& text, const QString& replyToId) {
  const QString convId = chat_->conversationId();
  if (convId.isEmpty()) {
    chat_->setStatus(QStringLiteral("先选一个会话"), true);
    return;
  }

  QVariantMap echo;
  echo.insert(QStringLiteral("conv_id"), convId);
  echo.insert(QStringLiteral("id"), QStringLiteral("local-%1").arg(QDateTime::currentMSecsSinceEpoch()));
  echo.insert(QStringLiteral("sender"), QVariantMap{{QStringLiteral("id"), client_->selfId()},
                                                     {QStringLiteral("display_name"),
                                                      QStringLiteral("我")}});
  echo.insert(QStringLiteral("content"),
              QVariantList{QVariantMap{{QStringLiteral("type"), QStringLiteral("text")},
                                      {QStringLiteral("text"), text}}});
  echo.insert(QStringLiteral("reply_to"), replyToId.isEmpty() ? QVariant() : replyToId);
  echo.insert(QStringLiteral("created_at"),
              static_cast<double>(QDateTime::currentMSecsSinceEpoch()));
  echo.insert(QStringLiteral("revoked_at"), QVariant());
  echo.insert(QStringLiteral("edited_at"), QVariant());
  echo.insert(QStringLiteral("state"), QStringLiteral("final"));
  chat_->appendLocalEcho(echo);

  QVariantMap data;
  data.insert(QStringLiteral("conv_id"), convId);
  data.insert(QStringLiteral("client_msg_id"),
              QStringLiteral("qt-%1").arg(QDateTime::currentMSecsSinceEpoch()));
  data.insert(QStringLiteral("content"),
              QVariantList{QVariantMap{{QStringLiteral("type"), QStringLiteral("text")},
                                      {QStringLiteral("text"), text}}});
  if (!replyToId.isEmpty()) data.insert(QStringLiteral("reply_to"), replyToId);

  client_->send(QStringLiteral("message.send"), data,
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

void MainWindow::onRevokeRequested(const QString& messageId) {
  const QString convId = chat_->conversationId();
  if (convId.isEmpty()) return;
  client_->send(QStringLiteral("message.revoke"),
                QVariantMap{{QStringLiteral("conv_id"), convId},
                            {QStringLiteral("message_id"), messageId}},
                [this](bool ok, const QVariantMap&, const WireError& error) {
                  if (!ok) {
                    chat_->setStatus(QStringLiteral("撤回失败：%1").arg(error.message), true);
                  }
                });
}

void MainWindow::onInterruptRequested(const QString& messageId) {
  const QString convId = chat_->conversationId();
  if (convId.isEmpty()) return;
  client_->send(QStringLiteral("stream.interrupt"),
                QVariantMap{{QStringLiteral("conv_id"), convId},
                            {QStringLiteral("message_id"), messageId}},
                [this](bool ok, const QVariantMap&, const WireError& error) {
                  if (!ok) {
                    chat_->setStatus(QStringLiteral("打断失败：%1").arg(error.message), true);
                  }
                });
}

void MainWindow::onImageRequested(const QString& attachmentId) {
  const ServerEntry* entry = settings_->activeEntry();
  const QString token = entry == nullptr ? QString() : entry->accessToken;
  if (token.isEmpty()) return;

  const QString tmpDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
  const QString dest = tmpDir + QStringLiteral("/agentlink-full-") + attachmentId.mid(0, 8) + QStringLiteral(".bin");

  api_->downloadAttachment(token, attachmentId, QStringLiteral("image"), dest,
                           [this, dest](bool ok, const QString&) {
                             if (!ok) {
                               chat_->setStatus(QStringLiteral("图片加载失败"), true);
                               return;
                             }
                             QPixmap pix(dest);
                             QFile::remove(dest);
                             if (pix.isNull()) {
                               chat_->setStatus(QStringLiteral("图片加载失败"), true);
                               return;
                             }

                             auto* dialog = new QDialog(this);
                             dialog->setAttribute(Qt::WA_DeleteOnClose);
                             dialog->setWindowTitle(QStringLiteral("查看图片"));
                             dialog->resize(720, 560);
                             auto* layout = new QVBoxLayout(dialog);
                             auto* area = new QScrollArea(dialog);
                             auto* label = new QLabel(area);
                             label->setPixmap(pix.scaled(680, 680, Qt::KeepAspectRatio, Qt::SmoothTransformation));
                             label->setAlignment(Qt::AlignCenter);
                             area->setWidget(label);
                             area->setWidgetResizable(true);
                             layout->addWidget(area);
                             dialog->show();
                           });
}

void MainWindow::onFileRequested(const QString& attachmentId, const QString& filename) {
  const ServerEntry* entry = settings_->activeEntry();
  const QString token = entry == nullptr ? QString() : entry->accessToken;
  if (token.isEmpty()) return;

  const QString name = filename.isEmpty() ? attachmentId : filename;
  const QString tmpDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
  const QString dest = tmpDir + QStringLiteral("/") + name;

  api_->downloadAttachment(token, attachmentId, name, dest,
                           [this, dest, name](bool ok, const QString&) {
                             if (!ok) {
                               chat_->setStatus(QStringLiteral("下载失败"), true);
                               return;
                             }
                             QDesktopServices::openUrl(QUrl::fromLocalFile(dest));
                             chat_->setStatus(QStringLiteral("已打开 %1").arg(name));
                           });
}

void MainWindow::onCreateGroup() {
  CreateGroupDialog dialog(this);
  if (dialog.exec() != QDialog::Accepted) return;

  QVariantMap data;
  data.insert(QStringLiteral("kind"), QStringLiteral("group"));
  const QString title = dialog.title();
  if (!title.isEmpty()) data.insert(QStringLiteral("title"), title);
  const QStringList memberIds = dialog.memberIds();
  if (!memberIds.isEmpty()) data.insert(QStringLiteral("member_ids"), memberIds);

  client_->send(QStringLiteral("conv.create"), data,
                [this](bool ok, const QVariantMap& res, const WireError& error) {
                  if (!ok) {
                    chat_->setStatus(QStringLiteral("建群失败：%1").arg(error.message), true);
                    return;
                  }
                  const QVariantMap conv = res.value(QStringLiteral("conv")).toMap();
                  const QString convId = conv.value(QStringLiteral("id")).toString();
                  convTitle_ = conv.value(QStringLiteral("title")).toString();
                  chat_->openConversation(convId, convTitle_);
                  refreshConversations();
                });
}

void MainWindow::onOpenGroupInfo() {
  const QString convId = chat_->conversationId();
  if (convId.isEmpty()) {
    chat_->setStatus(QStringLiteral("先选一个会话"), true);
    return;
  }
  GroupInfoDialog dialog(convTitle_, convAnnouncement_, this);
  if (dialog.exec() != QDialog::Accepted) return;
  saveGroupInfo(dialog.title(), dialog.announcement());
}

void MainWindow::onOpenMembers() {
  const QString convId = chat_->conversationId();
  if (convId.isEmpty()) {
    chat_->setStatus(QStringLiteral("先选一个会话"), true);
    return;
  }
  MembersDialog dialog(client_, convId, client_->selfId(), members_, this);
  dialog.exec();
  // 对话框里改过成员之后，主窗口缓存的那份要作废 —— 否则下次打开还是旧的
  client_->send(QStringLiteral("conv.get"), QVariantMap{{QStringLiteral("conv_id"), convId}},
                [this](bool ok, const QVariantMap& data, const WireError&) {
                  if (ok) members_ = data.value(QStringLiteral("members")).toList();
                });
}

void MainWindow::saveGroupInfo(const QString& title, const QString& announcement) {
  const QString convId = chat_->conversationId();
  if (convId.isEmpty()) return;

  QVariantMap data;
  data.insert(QStringLiteral("conv_id"), convId);
  data.insert(QStringLiteral("title"), title);
  data.insert(QStringLiteral("announcement"), announcement);

  client_->send(QStringLiteral("group.update"), data,
                [this](bool ok, const QVariantMap& res, const WireError& error) {
                  if (!ok) {
                    chat_->setStatus(QStringLiteral("保存群信息失败：%1").arg(error.message), true);
                    return;
                  }
                  const QVariantMap conv = res.value(QStringLiteral("conv")).toMap();
                  convTitle_ = conv.value(QStringLiteral("title")).toString();
                  convAnnouncement_ = conv.value(QStringLiteral("announcement")).toString();
                  refreshConversations();
                });
}

void MainWindow::onToggleSearch() {
  const bool open = searchPanel_->isHidden();
  searchPanel_->setVisible(open);
  conversations_->setVisible(!open);
}

void MainWindow::onJumpToMessage(const QString& convId, const QString& messageId, qint64 seq) {
  // 关搜索面板，切到目标会话
  searchPanel_->hide();
  conversations_->setVisible(true);
  if (!convId.isEmpty()) {
    convTitle_ = QString();
    chat_->openConversation(convId, convTitle_);
  }
  Q_UNUSED(messageId)
  Q_UNUSED(seq)
}

void MainWindow::onEditRequested(const QString& messageId) {
  const QString convId = chat_->conversationId();
  if (convId.isEmpty()) return;

  // 拿到原文再改（编辑对话框要预填）
  bool ok = false;
  const QString newText = QInputDialog::getMultiLineText(
      this, QStringLiteral("编辑消息"), QStringLiteral("新内容"), QString(), &ok);
  if (!ok || newText.trimmed().isEmpty()) return;

  client_->send(QStringLiteral("message.edit"),
                QVariantMap{{QStringLiteral("conv_id"), convId},
                            {QStringLiteral("message_id"), messageId},
                            {QStringLiteral("content"),
                             QVariantList{QVariantMap{{QStringLiteral("type"), QStringLiteral("text")},
                                                     {QStringLiteral("text"), newText}}}}},
                [this](bool ok2, const QVariantMap&, const WireError& error) {
                  if (!ok2) {
                    chat_->setStatus(QStringLiteral("编辑失败：%1").arg(error.message), true);
                  }
                });
}

void MainWindow::onForwardRequested(const QString& messageId) {
  forwardMessage(messageId);
}

void MainWindow::forwardMessage(const QString& messageId) {
  const QString fromConvId = chat_->conversationId();
  if (fromConvId.isEmpty()) return;

  client_->send(QStringLiteral("conv.list"), QVariantMap(),
                [this, messageId, fromConvId](bool ok, const QVariantMap& data, const WireError& error) {
                  if (!ok) {
                    chat_->setStatus(QStringLiteral("取会话列表失败：%1").arg(error.message), true);
                    return;
                  }
                  QStringList titles;
                  QStringList ids;
                  const QVariantList items = data.value(QStringLiteral("items")).toList();
                  for (const QVariant& it : items) {
                    const QVariantMap conv = it.toMap();
                    ids << conv.value(QStringLiteral("id")).toString();
                    titles << conv.value(QStringLiteral("title")).toString();
                  }
                  if (ids.isEmpty()) {
                    chat_->setStatus(QStringLiteral("没有可转发到的会话"), true);
                    return;
                  }

                  bool pickedOk = false;
                  const QString picked = QInputDialog::getItem(
                      this, QStringLiteral("转发"), QStringLiteral("转发到哪个会话？"),
                      titles, 0, false, &pickedOk);
                  if (!pickedOk) return;
                  const int idx = titles.indexOf(picked);
                  if (idx < 0) return;
                  const QString targetId = ids.at(idx);

                  client_->send(QStringLiteral("message.forward"),
                                QVariantMap{{QStringLiteral("conv_id"), targetId},
                                            {QStringLiteral("from_conv_id"), fromConvId},
                                            {QStringLiteral("message_id"), messageId}},
                                [this](bool ok2, const QVariantMap&, const WireError& err) {
                                  if (!ok2) {
                                    chat_->setStatus(QStringLiteral("转发失败：%1").arg(err.message), true);
                                  } else {
                                    chat_->setStatus(QStringLiteral("已转发"));
                                  }
                                });
                });
}

void MainWindow::onPinRequested(const QString& messageId) {
  const QString convId = chat_->conversationId();
  if (convId.isEmpty()) return;
  client_->send(QStringLiteral("message.pin"),
                QVariantMap{{QStringLiteral("conv_id"), convId},
                            {QStringLiteral("message_id"), messageId}},
                [this](bool ok, const QVariantMap&, const WireError& error) {
                  if (!ok) {
                    chat_->setStatus(QStringLiteral("置顶失败：%1").arg(error.message), true);
                  } else {
                    chat_->setStatus(QStringLiteral("已置顶"));
                  }
                });
}

void MainWindow::onReactionRequested(const QString& messageId, const QString& emoji, bool remove) {
  const QString convId = chat_->conversationId();
  if (convId.isEmpty()) return;
  client_->send(remove ? QStringLiteral("reaction.remove") : QStringLiteral("reaction.add"),
                QVariantMap{{QStringLiteral("conv_id"), convId},
                            {QStringLiteral("message_id"), messageId},
                            {QStringLiteral("emoji"), emoji}},
                [this](bool ok, const QVariantMap&, const WireError& error) {
                  if (!ok) {
                    chat_->setStatus(QStringLiteral("回应失败：%1").arg(error.message), true);
                  }
                });
}

void MainWindow::onInputTextChanged() {
  // 节流：服务端 TYPING_THROTTLE_MS = 2000，客户端也按 2 秒窗口报一次。
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  if (now - lastTypingSent_ < 2000) return;
  const QString convId = chat_->conversationId();
  if (convId.isEmpty()) return;
  lastTypingSent_ = now;
  client_->send(QStringLiteral("typing.send"), QVariantMap{{QStringLiteral("conv_id"), convId}},
                [](bool, const QVariantMap&, const WireError&) {});
}

void MainWindow::reportRead(qint64 lastSeq) {
  const QString convId = chat_->conversationId();
  if (convId.isEmpty() || lastSeq <= 0) return;
  // 已读水位：只报当前会话看到的最后一条终态消息的 seq。服务端只增不减，重复上报安全。
  client_->send(QStringLiteral("read.set"),
                QVariantMap{{QStringLiteral("conv_id"), convId}, {QStringLiteral("last_seq"), lastSeq}},
                [](bool, const QVariantMap&, const WireError&) {});
}

void MainWindow::requireLogin() {
  chat_->setStatus(QStringLiteral("登录已失效，请重新登录"), true);
}

}  // namespace agentlink
