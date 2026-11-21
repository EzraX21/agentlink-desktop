#include "client.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

namespace agentlink {
namespace {

/// 请求超时（毫秒）。协议本身没有规定，但无限等待会让界面看着像卡死。
constexpr int kRequestTimeoutMs = 30000;

/// 重连退避：第 n 次重连等 min(30000, 500 * 2^n) 毫秒。
int reconnectDelayMs(int attempt) {
  int delay = 500;
  for (int i = 0; i < attempt && delay < 30000; ++i) {
    delay *= 2;
  }
  return delay > 30000 ? 30000 : delay;
}

}  // namespace

std::optional<ServerAddress> ServerAddress::parse(const QString& input) {
  const QString trimmed = input.trimmed();
  if (trimmed.isEmpty()) return std::nullopt;

  // 补协议头：用户多半直接填 "chat.example.com" 或 "https://chat.example.com"。
  QString url = trimmed;
  if (!url.contains(QStringLiteral("://"))) {
    url = QStringLiteral("ws://") + url;
  }

  const QUrl parsed = QUrl::fromUserInput(url);
  if (!parsed.isValid() || parsed.host().isEmpty()) return std::nullopt;

  const QString scheme = parsed.scheme();
  const QString host = parsed.host();
  // 端口单独取。注意：不能从 path() 里剥端口 ——
  // QUrl::fromUserInput 对未知 scheme 会把 ":3000" 留在 path 里
  // （实测 "http://127.0.0.1:3000" → path == ":3000"），
  // 直接拼会得到 "ws://127.0.0.1/ws:3000" 这种废地址。
  const QString port = parsed.port() >= 0 ? QStringLiteral(":%1").arg(parsed.port()) : QString();

  const auto build = [&](const QString& wsScheme, const QString& path) {
    ServerAddress address;
    address.url = QStringLiteral("%1://%2%3%4").arg(wsScheme, host, port, path);
    // HTTP origin：只有 scheme + host + port，不含 /ws 路径
    address.origin = QStringLiteral("%1://%2%3").arg(
        wsScheme == QStringLiteral("wss") ? QStringLiteral("https") : QStringLiteral("http"), host, port);
    return address;
  };

  if (scheme == QStringLiteral("ws") || scheme == QStringLiteral("wss")) {
    QString path = parsed.path();
    if (path.isEmpty() || path == QStringLiteral("/")) {
      path = QStringLiteral("/ws");
    }
    return build(scheme, path);
  }

  // https/http 也接受：自动映射到 ws/wss（用户填的是浏览器地址栏那个）
  if (scheme == QStringLiteral("https")) {
    return build(QStringLiteral("wss"), QStringLiteral("/ws"));
  }
  if (scheme == QStringLiteral("http")) {
    return build(QStringLiteral("ws"), QStringLiteral("/ws"));
  }

  return std::nullopt;
}

QString ServerAddress::display() const {
  QString s = url;
  if (s.startsWith(QStringLiteral("ws://"))) s = s.mid(5);
  if (s.startsWith(QStringLiteral("wss://"))) s = s.mid(6);
  return s;
}

QString ServerAddress::httpBase() const {
  if (!origin.isEmpty()) return origin;
  // 兜底：从 url 里推（origin 应该有值，但别让空 origin 变成空 base）
  QString s = url;
  if (s.startsWith(QStringLiteral("ws://"))) return QStringLiteral("http://") + s.mid(5);
  if (s.startsWith(QStringLiteral("wss://"))) return QStringLiteral("https://") + s.mid(6);
  return s;
}

Client::Client(QObject* parent) : QObject(parent) {
  // 版本用默认（VersionLatest）：QWebSocket 自己处理协议协商。
  socket_ = new QWebSocket();
  socket_->setParent(this);

  reconnectTimer_ = new QTimer(this);
  reconnectTimer_->setSingleShot(true);

  authTimer_ = new QTimer(this);
  authTimer_->setSingleShot(true);
  authTimer_->setInterval(kAuthTimeoutMs);

  connect(socket_, &QWebSocket::connected, this, [this] {
    // 连接建立 → 立刻认证（协议 §2）。
    setState(ConnectionState::Authenticating, QStringLiteral("已连接，正在认证"));
    authTimer_->start();
    sendAuth();
  });

  connect(socket_, &QWebSocket::textMessageReceived, this, &Client::handleText);
  connect(socket_, &QWebSocket::disconnected, this, &Client::handleDisconnect);

  connect(reconnectTimer_, &QTimer::timeout, this, [this] {
    if (userRequestedDisconnect_) return;
    socket_->open(QUrl(address_.url));
  });

  connect(authTimer_, &QTimer::timeout, this, [this] {
    if (state_ != ConnectionState::Authenticating) return;
    setState(ConnectionState::Disconnected, QStringLiteral("认证超时"));
    socket_->abort();
  });
}

Client::~Client() {
  userRequestedDisconnect_ = true;
  for (auto it = pending_.begin(); it != pending_.end(); ++it) {
    if (it->timeout != nullptr) it->timeout->stop();
  }
  pending_.clear();
}

void Client::setState(ConnectionState state, const QString& detail) {
  state_ = state;
  if (stateHandler_) stateHandler_(state, detail);
  emit stateChanged(static_cast<int>(state), detail);
}

void Client::connectTo(const ServerAddress& address, const QString& token) {
  address_ = address;
  token_ = token;
  userRequestedDisconnect_ = false;
  reconnectAttempt_ = 0;

  if (socket_->state() != QAbstractSocket::UnconnectedState) {
    socket_->abort();
  }
  setState(ConnectionState::Connecting, QStringLiteral("正在连接 %1").arg(address.display()));

  // 带上子协议（施工守则 §7）：服务端会校验。
  // 注意：wss 的 TLS 配置交给 QWebSocket 默认策略（系统 CA）。
  // 不在这里关掉证书校验 —— 聊天内容走明文已经够糟了，不能再接受自签证书。
  socket_->open(QUrl(address_.url));
}

void Client::disconnectFromServer() {
  userRequestedDisconnect_ = true;
  reconnectTimer_->stop();
  authTimer_->stop();
  socket_->abort();
  setState(ConnectionState::Disconnected, QStringLiteral("已断开"));
}

void Client::sendAuth() {
  QVariantMap data;
  data.insert(QStringLiteral("token"), token_);
  const QString id = send(QStringLiteral("auth"), data, [this](bool ok, const QVariantMap& res, const WireError& err) {
    authTimer_->stop();
    if (!ok) {
      // 认证失败不该重连 —— token 错了，重连多少次都一样。
      userRequestedDisconnect_ = true;
      setState(ConnectionState::Disconnected,
               QStringLiteral("认证失败：%1").arg(err.message));
      if (errorHandler_) errorHandler_(err);
      socket_->abort();
      return;
    }
    // auth 响应是 { protocol, self: {...}, server_time } ——
    // self_ 要取的是 **self** 子对象，不是整个响应（取错了 selfId 永远是空）。
    self_ = res.value(QStringLiteral("self")).toMap();
    reconnectAttempt_ = 0;
    setState(ConnectionState::Ready, QStringLiteral("已连接"));
    // 断线期间的消息靠 resume 补齐（协议 §3.3）。
    if (lastSeq_ > 0) sendResume();
  });
  Q_UNUSED(id)
}

void Client::sendResume() {
  QVariantMap data;
  data.insert(QStringLiteral("last_seq"), lastSeq_);
  send(QStringLiteral("resume"), data, nullptr);
}

QString Client::send(const QString& op, const QVariantMap& data, ReplyHandler onReply) {
  ++nextId_;
  const QString id = QStringLiteral("q%1").arg(nextId_);

  if (socket_ == nullptr || socket_->state() != QAbstractSocket::ConnectedState) {
    // 不在连接状态：立刻回失败，不要让调用方干等。
    if (onReply) {
      WireError err;
      err.code = err::kUnauthenticated;
      err.message = QStringLiteral("未连接到服务端");
      onReply(false, {}, err);
    }
    return id;
  }

  const QString text = buildRequest(id, op, data);
  socket_->sendTextMessage(text);

  if (onReply) {
    Pending pending;
    pending.handler = std::move(onReply);
    pending.timeout = new QTimer(this);
    pending.timeout->setSingleShot(true);
    connect(pending.timeout, &QTimer::timeout, this, [this, id, op] {
      auto it = pending_.find(id);
      if (it == pending_.end()) return;
      Pending p = it.value();
      pending_.erase(it);
      p.timeout->stop();
      p.timeout->deleteLater();
      WireError err;
      err.code = QStringLiteral("TIMEOUT");
      err.message = QStringLiteral("请求超时：%1").arg(op);
      if (errorHandler_) errorHandler_(err);
      if (p.handler) p.handler(false, {}, err);
    });
    pending.timeout->start(kRequestTimeoutMs);
    pending_.insert(id, pending);
  }

  return id;
}

void Client::cancel(const QString& requestId) {
  auto it = pending_.find(requestId);
  if (it == pending_.end()) return;
  if (it->timeout != nullptr) {
    it->timeout->stop();
    it->timeout->deleteLater();
  }
  pending_.erase(it);
}

void Client::handleText(const QString& text) {
  const ParseResult parsed = parseFrame(text);
  if (!parsed.ok) {
    // 协议里说：坏帧不该断开连接（协议 §3.2）。
    if (errorHandler_) {
      WireError err;
      err.code = QStringLiteral("BAD_FRAME");
      err.message = parsed.error;
      errorHandler_(err);
    }
    return;
  }
  handleFrame(parsed.frame);
}

void Client::handleFrame(const Frame& frame) {
  switch (frame.kind) {
    case FrameKind::Response: {
      auto it = pending_.find(frame.id);
      if (it == pending_.end()) {
        // 未知 id：可能是超时后才回来的。不算错误。
        return;
      }
      Pending p = it.value();
      pending_.erase(it);
      if (p.timeout != nullptr) {
        p.timeout->stop();
        p.timeout->deleteLater();
      }
      if (p.handler) p.handler(frame.ok, frame.result, frame.error);
      return;
    }
    case FrameKind::Event: {
      // 全局 seq 只增不减（协议 §3.3）：用来做 resume。
      if (frame.seq.has_value() && *frame.seq > lastSeq_) {
        lastSeq_ = *frame.seq;
      }
      if (eventHandler_) eventHandler_(frame);
      emit eventReceived(frame.type, frame.eventData);
      return;
    }
    case FrameKind::Request:
      // 服务端不会向客户端发请求（协议 §3）。收到就忽略。
      return;
  }
}

void Client::handleDisconnect() {
  authTimer_->stop();

  // 所有在途请求都失败：不能让他们永远等。
  const auto pendingCopy = pending_;
  pending_.clear();
  for (auto it = pendingCopy.constBegin(); it != pendingCopy.constEnd(); ++it) {
    if (it->timeout != nullptr) {
      it->timeout->stop();
      it->timeout->deleteLater();
    }
    WireError err;
    err.code = err::kUnauthenticated;
    err.message = QStringLiteral("连接断开");
    if (errorHandler_) errorHandler_(err);
    if (it->handler) it->handler(false, {}, err);
  }

  if (userRequestedDisconnect_) {
    setState(ConnectionState::Disconnected, QStringLiteral("已断开"));
    return;
  }

  setState(ConnectionState::Disconnected, QStringLiteral("连接断开，正在重连…"));
  scheduleReconnect();
}

void Client::scheduleReconnect() {
  if (!autoReconnect_) return;
  if (reconnectTimer_->isActive()) return;
  const int delay = reconnectDelayMs(reconnectAttempt_);
  ++reconnectAttempt_;
  reconnectTimer_->start(delay);
  // 重连时 token 可能已过期，但服务端在 auth 失败时会明确告诉客户端。
  // 到那时再让用户重新登录，不要在这里自作主张。
}

}  // namespace agentlink
