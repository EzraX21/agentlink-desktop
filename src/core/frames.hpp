#pragma once

/**
 * AgentLink 协议核心。
 *
 * ## 为什么单独一层
 *
 * GUI 那层（Qt Widgets）不好单测：跑一次要显示服务，CI 里很别扭。
 * 但协议逻辑（帧编解码、请求路由、事件分发、断线重连）是最容易出错、
 * 也最值得测的部分。
 *
 * 所以把它拆成不依赖 GUI 的 `agentlink_core`：
 * 只依赖 QtCore（JSON、事件循环）与 QtNetwork（WebSocket、HTTP）。
 * 这样验收脚本可以**不起窗口**就把协议层跑一遍 ——
 * 而且它连的是**真的** AgentLink 服务端。
 *
 * ## 依赖说明
 *
 * 用 Qt 自带的 QJsonDocument 与 QWebSocket，而不是引入 nlohmann/json +
 * websocketpp：
 *   - QJsonDocument 在 QtCore 里，装 Qt 就有，零额外依赖
 *   - QWebSocket 处理了 RFC 6455 的全部细节（掩码、分片、ping/pong、续帧）
 *   - 手写 WebSocket 客户端是**几百行**的坑（分片重组、掩码、关闭握手），
 *     换来的只是"少一个依赖" —— 不划算
 */

#include <QJsonObject>
#include <QString>
#include <QVariantMap>
#include <optional>

namespace agentlink {

/// 协议版本（协议 §1）。
inline constexpr int kProtocolVersion = 1;

/// WS 子协议标识（施工守则 §7）。
inline const QString kWsSubprotocol = QStringLiteral("agentlink.v1");

/// 单帧上限 1 MiB（协议 §1）。
inline constexpr int kMaxFrameBytes = 1024 * 1024;

/// 未认证连接的握手超时（协议 §2）。
inline constexpr int kAuthTimeoutMs = 5000;

/// 一条线上的错误（协议 §5）。
struct WireError {
  QString code;
  QString message;
  /// 原始 details（JSON 文本）。多数时候用不上，但排障时很值钱。
  QString detailsJson;
};

/// 帧类型（协议 §3）。只有三种。
enum class FrameKind {
  Request,
  Response,
  Event,
};

/// 解析出来的一帧。
struct Frame {
  FrameKind kind = FrameKind::Request;

  // --- Request ---
  QString id;
  QString op;
  QVariantMap data;  ///< 没有 data 时是空 map

  // --- Response ---
  bool ok = false;
  WireError error;
  QVariantMap result;  ///< ok 时是 data

  // --- Event ---
  QString type;
  /// 瞬时事件为 nullopt（协议 §4.1）。
  std::optional<qint64> seq;
  qint64 ts = 0;
  QVariantMap eventData;
};

/// 帧解析结果。
struct ParseResult {
  bool ok = false;
  Frame frame;
  QString error;  ///< 失败原因
};

/// 解析一帧文本。
///
/// 只做**信封层**校验（协议 §3）：kind / id / op / type / seq 的形状。
/// `data` 的具体结构由各 op 自己管 —— 这样未知 op 也能先解出信封，
/// 再回 `UNSUPPORTED_OP`（协议 §5）。
ParseResult parseFrame(const QString& text);

/// 构造一个请求帧的 JSON 文本。
QString buildRequest(const QString& id, const QString& op, const QVariantMap& data);

/// 构造一个响应帧的 JSON 文本（给测试与假服务端用）。
QString buildResponseOk(const QString& id, const QVariantMap& data);
QString buildResponseErr(const QString& id, const QString& code, const QString& message);

/// 构造一个事件帧的 JSON 文本。
QString buildEvent(const QString& type, const std::optional<qint64>& seq, qint64 ts,
                   const QVariantMap& data);

/// 错误码常量（协议 §5）。
namespace err {
inline const QString kUnauthenticated = QStringLiteral("UNAUTHENTICATED");
inline const QString kPermissionDenied = QStringLiteral("PERM_DENIED");
inline const QString kNotFound = QStringLiteral("NOT_FOUND");
inline const QString kValidationFailed = QStringLiteral("VALIDATION_FAILED");
inline const QString kUnsupportedOp = QStringLiteral("UNSUPPORTED_OP");
inline const QString kRateLimited = QStringLiteral("RATE_LIMITED");
inline const QString kPayloadTooLarge = QStringLiteral("PAYLOAD_TOO_LARGE");
inline const QString kInternal = QStringLiteral("INTERNAL");
}  // namespace err

}  // namespace agentlink
