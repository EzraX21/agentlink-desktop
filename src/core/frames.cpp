#include "frames.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

namespace agentlink {
namespace {

/// 把 data 字段读成 map。协议里 data 永远是对象；不是就当空 map。
QVariantMap toMap(const QJsonValue& v) {
  if (!v.isObject()) return {};
  return v.toObject().toVariantMap();
}

QJsonObject fromMap(const QVariantMap& m) {
  return QJsonObject::fromVariantMap(m);
}

/// 单帧上限检查（协议 §1）。
bool withinLimit(const QString& text) {
  return text.toUtf8().size() <= static_cast<size_t>(kMaxFrameBytes);
}

}  // namespace

ParseResult parseFrame(const QString& text) {
  ParseResult result;
  if (!withinLimit(text)) {
    result.error = QStringLiteral("帧超过 %1 字节上限").arg(kMaxFrameBytes);
    return result;
  }

  QJsonParseError parseError{};
  const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
    result.error = QStringLiteral("不是合法 JSON 对象：%1").arg(parseError.errorString());
    return result;
  }

  const QJsonObject obj = doc.object();
  const QString kind = obj.value(QStringLiteral("kind")).toString();
  const QJsonValue version = obj.value(QStringLiteral("v"));

  // 版本不符直接拒（协议 §3.1：未知版本回 UNSUPPORTED_VERSION）。
  // 这里只做形状判断，具体的错误码由上层决定。
  if (!version.isDouble()) {
    result.error = QStringLiteral("缺少协议版本字段 v");
    return result;
  }

  if (kind == QStringLiteral("req")) {
    Frame frame;
    frame.kind = FrameKind::Request;
    frame.id = obj.value(QStringLiteral("id")).toString();
    frame.op = obj.value(QStringLiteral("op")).toString();
    if (frame.id.isEmpty()) {
      result.error = QStringLiteral("请求帧缺少 id");
      return result;
    }
    if (frame.op.isEmpty()) {
      result.error = QStringLiteral("请求帧缺少 op");
      return result;
    }
    frame.data = toMap(obj.value(QStringLiteral("data")));
    result.ok = true;
    result.frame = frame;
    return result;
  }

  if (kind == QStringLiteral("res")) {
    Frame frame;
    frame.kind = FrameKind::Response;
    frame.id = obj.value(QStringLiteral("id")).toString();
    const QJsonValue okValue = obj.value(QStringLiteral("ok"));
    frame.ok = okValue.toBool(false);
    if (frame.ok) {
      frame.result = toMap(obj.value(QStringLiteral("data")));
    } else {
      const QJsonObject error = obj.value(QStringLiteral("error")).toObject();
      frame.error.code = error.value(QStringLiteral("code")).toString();
      frame.error.message = error.value(QStringLiteral("message")).toString();
      const QJsonValue details = error.value(QStringLiteral("details"));
      if (!details.isUndefined() && !details.isNull()) {
        // details 可能是对象/数组/标量。Qt 6.2 的 QJsonDocument 只有
        // QJsonObject / QJsonArray 两种构造，所以标量单独处理。
        QJsonDocument doc;
        if (details.isObject()) {
          doc = QJsonDocument(details.toObject());
        } else if (details.isArray()) {
          doc = QJsonDocument(details.toArray());
        } else {
          // 标量包成一个对象存（键名固定），至少不丢信息
          QJsonObject wrapper;
          wrapper.insert(QStringLiteral("value"), details);
          doc = QJsonDocument(wrapper);
        }
        frame.error.detailsJson = QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
      }
    }
    result.ok = true;
    result.frame = frame;
    return result;
  }

  if (kind == QStringLiteral("evt")) {
    Frame frame;
    frame.kind = FrameKind::Event;
    frame.type = obj.value(QStringLiteral("type")).toString();
    const QJsonValue seqValue = obj.value(QStringLiteral("seq"));
    // 瞬时事件 seq 为 null（协议 §4.1）—— 必须区分"null"与"缺失"：
    // 缺失是非法的，null 是合法的瞬时标记。
    if (seqValue.isNull()) {
      frame.seq = std::nullopt;
    } else if (seqValue.isDouble()) {
      frame.seq = static_cast<qint64>(seqValue.toDouble());
    } else {
      result.error = QStringLiteral("事件帧的 seq 必须是数字或 null");
      return result;
    }
    frame.ts = static_cast<qint64>(obj.value(QStringLiteral("ts")).toDouble());
    frame.eventData = toMap(obj.value(QStringLiteral("data")));
    result.ok = true;
    result.frame = frame;
    return result;
  }

  result.error = QStringLiteral("未知帧类型：%1").arg(kind);
  return result;
}

QString buildRequest(const QString& id, const QString& op, const QVariantMap& data) {
  QJsonObject obj;
  obj.insert(QStringLiteral("v"), kProtocolVersion);
  obj.insert(QStringLiteral("kind"), QStringLiteral("req"));
  obj.insert(QStringLiteral("id"), id);
  obj.insert(QStringLiteral("op"), op);
  if (!data.isEmpty()) {
    obj.insert(QStringLiteral("data"), fromMap(data));
  }
  return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

QString buildResponseOk(const QString& id, const QVariantMap& data) {
  QJsonObject obj;
  obj.insert(QStringLiteral("v"), kProtocolVersion);
  obj.insert(QStringLiteral("kind"), QStringLiteral("res"));
  obj.insert(QStringLiteral("id"), id);
  obj.insert(QStringLiteral("ok"), true);
  obj.insert(QStringLiteral("data"), fromMap(data));
  return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

QString buildResponseErr(const QString& id, const QString& code, const QString& message) {
  QJsonObject error;
  error.insert(QStringLiteral("code"), code);
  error.insert(QStringLiteral("message"), message);

  QJsonObject obj;
  obj.insert(QStringLiteral("v"), kProtocolVersion);
  obj.insert(QStringLiteral("kind"), QStringLiteral("res"));
  obj.insert(QStringLiteral("id"), id);
  obj.insert(QStringLiteral("ok"), false);
  obj.insert(QStringLiteral("error"), error);
  return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

QString buildEvent(const QString& type, const std::optional<qint64>& seq, qint64 ts,
                   const QVariantMap& data) {
  QJsonObject obj;
  obj.insert(QStringLiteral("v"), kProtocolVersion);
  obj.insert(QStringLiteral("kind"), QStringLiteral("evt"));
  obj.insert(QStringLiteral("type"), type);
  if (seq.has_value()) {
    obj.insert(QStringLiteral("seq"), static_cast<double>(*seq));
  } else {
    obj.insert(QStringLiteral("seq"), QJsonValue::Null);
  }
  obj.insert(QStringLiteral("ts"), static_cast<double>(ts));
  obj.insert(QStringLiteral("data"), fromMap(data));
  return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

}  // namespace agentlink
