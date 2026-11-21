#pragma once

#include <QMainWindow>
#include <QVariantMap>

class QSplitter;
class QLabel;

namespace agentlink {

class Client;
class AuthApi;
class SettingsStore;
class ConversationList;
class ChatWindow;

/**
 * 主窗口：左会话列表 + 右聊天窗口。
 *
 * 它是"胶水层"：把 Client 的事件分发给两个面板，
 * 把面板的意图翻译成协议 op。
 */
class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  MainWindow(Client* client, AuthApi* api, SettingsStore* settings, QWidget* parent = nullptr);

  /// 登录态失效了：回到引导流程。
  void requireLogin();

  /// 用户要求加/换服务器（引导流程再走一遍）。
  void promptSwitchServer();

 signals:
  /// 连到了别的服务器：上层要用新地址与新 token 重连。
  void serverChanged(const QString& wsUrl, const QString& accessToken);

 private slots:
  void onStateChanged(int state, const QString& detail);
  void onEvent(const QString& type, const QVariantMap& data);
  void onSendRequested(const QString& text);
  void onAttachmentRequested();
  void onConversationSelected(const QString& convId);

 private:
  void buildMenus();
  void refreshConversations();
  void sendMessage(const QString& text);
  void sendAttachment(const QString& filePath);

  Client* client_ = nullptr;
  AuthApi* api_ = nullptr;
  SettingsStore* settings_ = nullptr;

  ConversationList* conversations_ = nullptr;
  ChatWindow* chat_ = nullptr;
  QSplitter* splitter_ = nullptr;

  QString convTitle_;
};

}  // namespace agentlink
