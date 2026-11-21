#pragma once

/**
 * AgentLink 连接（协议客户端）。
 *
 * 职责：
 *   1. 连 WebSocket（带 `agentlink.v1` 子协议）
 *   2. `auth` 握手（协议 §2）
 *   3. 请求/响应配对（按 `id`）
 *   4. 事件分发（按 `type`）
 *   5. 断线重连 + `resume` 补齐（协议 §3.3）
 *
 * ## 为什么自己写而不是用 SDK
 *
 * `@agentlink/agent-sdk` 是给 TypeScript agent 用的；这个是给 C++ 客户端用的。
 * 协议是同一个，但语言不同 —— 重写一遍比绕一大圈更清楚。
 *
 * ## 线程模型
 *
 * **全部在 GUI 线程**。QWebSocket 本身就是这个模型；
 * 桌面客户端的数据量（几百条消息、几个会话）根本不需要线程。
 */

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <functional>
#include <memory>
#include <optional>

#include "frames.hpp"

class QWebSocket;
class QTimer;

namespace agentlink {

/// 连接状态。
enum class ConnectionState {
  Disconnected,
  Connecting,
  Authenticating,
  Ready,
};

/// 服务端地址。
///
/// 支持两种写法：
///   - `ws://host:port`（明文）
///   - `https://host`（自动转成 `wss://host/ws`）
///
/// 为什么自动转：用户填的往往就是浏览器地址栏那个（`https://chat.example.com`），
/// 让他自己改成 `wss://` 只会制造困惑。
struct ServerAddress {
  QString url;  ///< 规范化后的 ws:// 或 wss:// 地址（含 /ws 路径）
  /// HTTP origin（`https://host[:port]`）。
  ///
  /// 与 `url` 分开存的原因：WebSocket 的路径（`/ws`）只对 WS 有意义，
  /// HTTP API 走的是 origin + `/api/...`。从 url 里剥路径容易出错，
  /// 解析时一次算清更可靠。
  QString origin;

  /// 从用户输入解析出规范地址。
  ///
  /// 返回空 optional 表示输入不合法（调用方负责给用户看得懂的提示）。
  static std::optional<ServerAddress> parse(const QString& input);

  /// 供界面显示用的"人类可读"形式（去掉 ws:// 前缀）。
  [[nodiscard]] QString display() const;

  /// 同源部署时，HTTP 基础地址（上传附件、注册登录用）。
  ///
  /// `ws://` → `http://`，`wss://` → `https://`。
  [[nodiscard]] QString httpBase() const;
};

/// 连接与协议交互。
class Client : public QObject {
  Q_OBJECT

 public:
  /// 回调：收到一个事件。
  using EventHandler = std::function<void(const Frame& frame)>;
  /// 回调：连接状态变了。
  using StateHandler = std::function<void(ConnectionState state, const QString& detail)>;
  /// 回调：一次请求失败了。
  using ErrorHandler = std::function<void(const WireError& error)>;

  explicit Client(QObject* parent = nullptr);
  ~Client() override;

  /// 连到某个服务端并完成认证。
  ///
  /// `token` 是 access token（人类）或 agent token。
  void connectTo(const ServerAddress& address, const QString& token);

  /// 主动断开（不再重连）。
  void disconnectFromServer();

  /// 当前状态。
  [[nodiscard]] ConnectionState state() const { return state_; }

  /// 当前连的服务端地址（没连过则为空）。
  [[nodiscard]] const ServerAddress& address() const { return address_; }

  /// 认证成功后拿到的自己的身份。
  [[nodiscard]] QVariantMap self() const { return self_; }

  /// 自己��� id（认证前为空）。
  [[nodiscard]] QString selfId() const { return self_.value(QStringLiteral("id")).toString(); }

  /// 发一个请求，返回请求 id（用于取消/追踪）。
  ///
  /// 回调 `onOk` / `onErr` 恰好有一个会被调用。
  using ReplyHandler = std::function<void(bool ok, const QVariantMap& data, const WireError& error)>;

  QString send(const QString& op, const QVariantMap& data, ReplyHandler onReply);

  /// 取消一个在途请求（回调不会被调用）。
  void cancel(const QString& requestId);

  /// 设置事件回调（单个，覆盖式）。
  void setEventHandler(EventHandler handler) { eventHandler_ = std::move(handler); }
  /// 设置状态回调。
  void setStateHandler(StateHandler handler) { stateHandler_ = std::move(handler); }
  /// 设置错误回调。
  void setErrorHandler(ErrorHandler handler) { errorHandler_ = std::move(handler); }

  /// 最后一次收到的全局 seq（用于 `resume` 补齐，协议 §3.3）。
  [[nodiscard]] qint64 lastSeq() const { return lastSeq_; }

  /// 是否开启了自动重连。
  void setAutoReconnect(bool enabled) { autoReconnect_ = enabled; }

 signals:
  /// 状态变了（给 QML/Qt 层用；C++ 侧也能用 setStateHandler）。
  void stateChanged(int state, const QString& detail);
  /// 收到事件。
  void eventReceived(const QString& type, const QVariantMap& data);

 private:
  struct Pending {
    ReplyHandler handler;
    QTimer* timeout{nullptr};
  };

  void setState(ConnectionState state, const QString& detail);
  void handleText(const QString& text);
  void handleFrame(const Frame& frame);
  void handleDisconnect();
  void scheduleReconnect();
  void sendAuth();
  void sendResume();

  QWebSocket* socket_ = nullptr;
  QTimer* reconnectTimer_ = nullptr;
  QTimer* authTimer_ = nullptr;

  ServerAddress address_;
  QString token_;
  ConnectionState state_ = ConnectionState::Disconnected;
  QVariantMap self_;

  int nextId_ = 0;
  QMap<QString, Pending> pending_;
  qint64 lastSeq_ = 0;

  bool autoReconnect_ = true;
  bool userRequestedDisconnect_ = false;
  int reconnectAttempt_ = 0;

  EventHandler eventHandler_;
  StateHandler stateHandler_;
  ErrorHandler errorHandler_;
};

}  // namespace agentlink
