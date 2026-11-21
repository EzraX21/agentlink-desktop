#include "auth_api.hpp"

#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace agentlink {
namespace {

/// 从错误响应里挖出人能看懂的话。
///
/// 服务端统一格式是 `{ error: { code, message, details } }`（协议 §5）。
/// 但网络层错误（DNS 失败、连不上）没有这个结构 —— 那时要给**另一种**提示，
/// 否则用户会看到"未知错误"而不知道是地址填错了。
QString extractError(const QVariantMap& body, const QString& fallback) {
  const QVariantMap error = body.value(QStringLiteral("error")).toMap();
  const QString message = error.value(QStringLiteral("message")).toString();
  return message.isEmpty() ? fallback : message;
}

QString extractErrorCode(const QVariantMap& body) {
  const QVariantMap error = body.value(QStringLiteral("error")).toMap();
  return error.value(QStringLiteral("code")).toString();
}

}  // namespace

AuthApi::AuthApi(QObject* parent) : QObject(parent) {
  manager_ = new QNetworkAccessManager(this);
}

QNetworkReply* AuthApi::postJson(const QString& path, const QVariantMap& body, const QString& token,
                                 std::function<void(int, const QVariantMap&, const QString&)> done) {
  QNetworkRequest request{QUrl(httpBase_ + path)};
  request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
  if (!token.isEmpty()) {
    request.setRawHeader("authorization", QByteArray("Bearer ") + token.toUtf8());
  }
  const QByteArray payload = QJsonDocument(QJsonObject::fromVariantMap(body)).toJson(QJsonDocument::Compact);
  QNetworkReply* reply = manager_->post(request, payload);
  finish(reply, std::move(done));
  return reply;
}

void AuthApi::finish(QNetworkReply* reply, std::function<void(int, const QVariantMap&, const QString&)> done) {
  connect(reply, &QNetworkReply::finished, this, [this, reply, done]() {
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray raw = reply->readAll();
    QVariantMap body;
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseError);
    if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
      body = doc.object().toVariantMap();
    }

    if (status == 0) {
      // 网络层失败：连都没连上。给一句能指向"地址填错了"的提示。
      const QString reason = reply->error() == QNetworkReply::HostNotFoundError
                                ? QStringLiteral("找不到这台服务器，请检查地址是否正确")
                                : QStringLiteral("连不上服务器：%1").arg(reply->errorString());
      done(0, body, reason);
    } else {
      done(status, body, QString::fromUtf8(raw));
    }
    reply->deleteLater();
    emit finished();
  });
}

void AuthApi::registerUser(const QString& username, const QString& displayName, const QString& password,
                           std::function<void(const AuthResult&)> callback) {
  QVariantMap body;
  body.insert(QStringLiteral("username"), username);
  body.insert(QStringLiteral("display_name"), displayName.isEmpty() ? username : displayName);
  body.insert(QStringLiteral("password"), password);

  postJson(QStringLiteral("/api/auth/register"), body, QString(),
           [callback](int status, const QVariantMap& res, const QString& raw) {
             AuthResult result;
             result.raw = res;
             if (status == 201) {
               result.ok = true;
               result.accessToken = res.value(QStringLiteral("access_token")).toString();
               result.refreshToken = res.value(QStringLiteral("refresh_token")).toString();
               const QVariantMap user = res.value(QStringLiteral("user")).toMap();
               result.userId = user.value(QStringLiteral("id")).toString();
               result.displayName = user.value(QStringLiteral("display_name")).toString();
             } else {
               result.error = extractError(res, QStringLiteral("注册失败（HTTP %1）").arg(status));
               result.errorCode = extractErrorCode(res);
               Q_UNUSED(raw)
             }
             if (callback) callback(result);
           });
}

void AuthApi::login(const QString& username, const QString& password,
                    std::function<void(const AuthResult&)> callback) {
  QVariantMap body;
  body.insert(QStringLiteral("username"), username);
  body.insert(QStringLiteral("password"), password);

  postJson(QStringLiteral("/api/auth/login"), body, QString(),
           [callback](int status, const QVariantMap& res, const QString& raw) {
             AuthResult result;
             result.raw = res;
             if (status == 200) {
               result.ok = true;
               result.accessToken = res.value(QStringLiteral("access_token")).toString();
               result.refreshToken = res.value(QStringLiteral("refresh_token")).toString();
               const QVariantMap user = res.value(QStringLiteral("user")).toMap();
               result.userId = user.value(QStringLiteral("id")).toString();
               result.displayName = user.value(QStringLiteral("display_name")).toString();
             } else {
               result.error = extractError(res, QStringLiteral("登录失败（HTTP %1）").arg(status));
               result.errorCode = extractErrorCode(res);
               Q_UNUSED(raw)
             }
             if (callback) callback(result);
           });
}

void AuthApi::refresh(const QString& refreshToken, std::function<void(const AuthResult&)> callback) {
  QVariantMap body;
  body.insert(QStringLiteral("refresh_token"), refreshToken);

  postJson(QStringLiteral("/api/auth/refresh"), body, QString(),
           [callback](int status, const QVariantMap& res, const QString& raw) {
             AuthResult result;
             result.raw = res;
             if (status == 200) {
               result.ok = true;
               result.accessToken = res.value(QStringLiteral("access_token")).toString();
               result.refreshToken = res.value(QStringLiteral("refresh_token")).toString();
             } else {
               result.error = extractError(res, QStringLiteral("刷新 token 失败（HTTP %1）").arg(status));
               result.errorCode = extractErrorCode(res);
               Q_UNUSED(raw)
             }
             if (callback) callback(result);
           });
}

void AuthApi::uploadAttachment(const QString& accessToken, const QString& filePath,
                               std::function<void(bool, const QVariantMap&, const QString&)> callback) {
  // 异步请求：multipart 必须在 reply 结束前一直活着，所以堆上分配并挂到 reply 上
  // （deleteLater 会一起回收）。堆变量在栈上销毁会让请求发出一半就崩。
  QHttpMultiPart* multi = new QHttpMultiPart(QHttpMultiPart::FormDataType);

  // QFile 也必须活到请求结束 —— 它是 multipart 的 body device。
  // 之前放在栈上：函数返回时 QFile 析构，网络层再读它就是野指针（实测 SIGSEGV）。
  // 所以堆分配 + 挂到 multipart 上（multipart 挂到 reply 上），生命周期跟着请求走。
  QFile* file = new QFile(filePath);
  if (!file->open(QIODevice::ReadOnly)) {
    const QString reason = file->errorString();
    delete file;
    delete multi;
    if (callback) callback(false, {}, QStringLiteral("打不开文件：%1").arg(reason));
    return;
  }

  QFileInfo info(filePath);
  QHttpPart part;
  part.setHeader(QNetworkRequest::ContentDispositionHeader,
                 QStringLiteral("form-data; name=\"file\"; filename=\"%1\"").arg(info.fileName()));
  part.setHeader(QNetworkRequest::ContentTypeHeader,
                 QStringLiteral("application/octet-stream"));
  part.setBodyDevice(file);
  multi->append(part);
  file->setParent(multi);  // multipart 释放时一起释放文件

  QNetworkRequest request{QUrl(httpBase_ + QStringLiteral("/api/attachments"))};
  request.setRawHeader("authorization", QByteArray("Bearer ") + accessToken.toUtf8());

  QNetworkReply* reply = manager_->post(request, multi);
  multi->setParent(reply);

  connect(reply, &QNetworkReply::finished, this, [this, reply, callback]() {
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray raw = reply->readAll();
    QVariantMap body;
    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (!doc.isNull() && doc.isObject()) body = doc.object().toVariantMap();

    if (status == 201) {
      if (callback) callback(true, body, QString());
    } else {
      if (callback) {
        callback(false, body,
                 extractError(body, QStringLiteral("上传失败（HTTP %1）").arg(status)));
      }
    }
    reply->deleteLater();
    emit finished();
  });
}

void AuthApi::downloadAttachment(const QString& accessToken, const QString& attachmentId,
                                 const QString& filename, const QString& destPath,
                                 std::function<void(bool, const QString&)> callback) {
  QNetworkRequest request{
      QUrl(httpBase_ + QStringLiteral("/api/attachments/%1/%2")
               .arg(QString::fromUtf8(QUrl::toPercentEncoding(attachmentId)),
                    QString::fromUtf8(QUrl::toPercentEncoding(filename.isEmpty() ? QStringLiteral("file") : filename))))};
  request.setRawHeader("authorization", QByteArray("Bearer ") + accessToken.toUtf8());

  QNetworkReply* reply = manager_->get(request);
  connect(reply, &QNetworkReply::finished, this, [this, reply, destPath, callback]() {
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 200) {
      QFile file(destPath);
      if (!file.open(QIODevice::WriteOnly)) {
        if (callback) callback(false, QStringLiteral("写不进目标文件：%1").arg(file.errorString()));
      } else {
        file.write(reply->readAll());
        file.close();
        if (callback) callback(true, QString());
      }
    } else {
      const QByteArray raw = reply->readAll();
      QVariantMap body;
      const QJsonDocument doc = QJsonDocument::fromJson(raw);
      if (!doc.isNull() && doc.isObject()) body = doc.object().toVariantMap();
      if (callback) {
        callback(false, extractError(body, QStringLiteral("下载失败（HTTP %1）").arg(status)));
      }
    }
    reply->deleteLater();
    emit finished();
  });
}

void AuthApi::fetchThumbnail(const QString& accessToken, const QString& attachmentId,
                             std::function<void(bool, const QByteArray&, const QString&)> callback) {
  QNetworkRequest request{QUrl(httpBase_ + QStringLiteral("/api/attachments/%1/thumbnail")
                                    .arg(QString::fromUtf8(QUrl::toPercentEncoding(attachmentId))))};
  request.setRawHeader("authorization", QByteArray("Bearer ") + accessToken.toUtf8());

  QNetworkReply* reply = manager_->get(request);
  connect(reply, &QNetworkReply::finished, this, [this, reply, callback]() {
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 200) {
      if (callback) callback(true, reply->readAll(), QString());
    } else {
      // 404 = 这个附件本来就没有缩略图（不是视频 / 服务端没装 ffmpeg）。
      // 这不是错误，调用方据此退回默认显示。
      if (callback) callback(false, QByteArray(), QString());
    }
    reply->deleteLater();
    emit finished();
  });
}

}  // namespace agentlink
