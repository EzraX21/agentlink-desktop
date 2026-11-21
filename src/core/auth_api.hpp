#pragma once

/**
 * HTTP 面：注册 / 登录 / 刷新 token / 上传附件。
 *
 * 为什么这些走 HTTP 而不是 WebSocket：
 * 协议里 `auth` op 是"用已有 token 握手"，**注册与登录**没有 WS 入口 ——
 * 服务端把账号相关的接口放在 /api/auth/ 下（需求确认书 §4）。
 *
 * 与 WebSocket 共用同一个 origin：地址栏那个 `https://chat.example.com`
 * 就是 HTTP base。
 */

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

namespace agentlink {

/// 账号相关的结果。
struct AuthResult {
  bool ok = false;
  QString error;      ///< 失败原因（给人看的）
  QString errorCode;  ///< 机器可读的码（如 VALIDATION_FAILED）
  QString accessToken;
  QString refreshToken;
  QString userId;
  QString displayName;
  /// 服务端拒绝时的完整响应（排障用）。
  QVariantMap raw;
};

/// HTTP 账号客户端。
class AuthApi : public QObject {
  Q_OBJECT

 public:
  explicit AuthApi(QObject* parent = nullptr);

  /// 设置服务端 HTTP 基础地址（`http://` 或 `https://`）。
  void setHttpBase(const QString& base) { httpBase_ = base; }
  [[nodiscard]] QString httpBase() const { return httpBase_; }

  /// 注册一个新账号。
  ///
  /// 密码至少 8 位（服务端要求），这里**不提前校验** ——
  /// 让服务端说了算，客户端与服务端的规则不一致比多一次请求更麻烦。
  void registerUser(const QString& username, const QString& displayName, const QString& password,
                    std::function<void(const AuthResult&)> callback);

  /// 登录。
  void login(const QString& username, const QString& password,
             std::function<void(const AuthResult&)> callback);

  /// 用 refresh token 换新的 access token。
  void refresh(const QString& refreshToken, std::function<void(const AuthResult&)> callback);

  /// 上传附件，返回 `{attachment_id, filename, mime, size, sha256}`。
  ///
  /// 附件要先上传再引用（协议 §7）。
  void uploadAttachment(const QString& accessToken, const QString& filePath,
                        std::function<void(bool ok, const QVariantMap& info, const QString& error)> callback);

  /// 下载附件到本地文件。
  void downloadAttachment(const QString& accessToken, const QString& attachmentId,
                          const QString& filename, const QString& destPath,
                          std::function<void(bool ok, const QString& error)> callback);

  /// 取视频缩略图（服务端上传时抽的帧）。没有则 ok=false。
  void fetchThumbnail(const QString& accessToken, const QString& attachmentId,
                      std::function<void(bool ok, const QByteArray& jpeg, const QString& error)> callback);

 signals:
  /// 请求完成（用于测试里等异步）。
  void finished();

 private:
  QNetworkReply* postJson(const QString& path, const QVariantMap& body, const QString& token,
                          std::function<void(int status, const QVariantMap& body, const QString& raw)> done);
  void finish(QNetworkReply* reply, std::function<void(int, const QVariantMap&, const QString&)> done);

  QNetworkAccessManager* manager_ = nullptr;
  QString httpBase_;
};

}  // namespace agentlink
