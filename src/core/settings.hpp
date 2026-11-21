#pragma once

/**
 * 本地设置（服务器地址 + 登录态）。
 *
 * ## 为什么存多个服务器
 *
 * 像 QQ / Element 那样，一个客户端可以连**多台** AgentLink 服务端
 * （公司一台、私人一台、测试环境一台）。所以这里存的是**列表**，
 * 不是单个地址。
 *
 * 每条记录：
 *   - `address`：服务端地址（用户手填）
 *   - `username` / `displayName`：这个服务端上的账号
 *   - `accessToken` / `refreshToken`：登录态
 *
 * ## 存哪
 *
 * `QStandardPaths::AppConfigLocation` —— Linux 上是
 * `~/.config/AgentLink/client.ini`（XDG 标准）。
 *
 * ## token 的安全性
 *
 * token 明文存在配置文件里。这是桌面客户端的常态做法（Element 也这样），
 * 代价是"能读你 home 目录的人能拿到你的聊天身份"。
 *
 * v1 不做系统钥匙串（那需要额外的平台依赖，且在 Linux 上要靠
 * libsecret / kwallet，各家还不一样）。把这件事写进文档，
 * 比假装没有更诚实。
 */

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <vector>

namespace agentlink {

/// 一个"已保存的服务器 + 账号"。
struct ServerEntry {
  /// 规范化后的 ws:// 或 wss:// 地址。
  QString address;
  /// 用户填的原始地址（显示用）。
  QString rawAddress;
  QString username;
  QString displayName;
  QString accessToken;
  QString refreshToken;
  /// 上次连的是不是这个（客户端启动时优先连它）。
  bool active = false;

  [[nodiscard]] bool isValid() const { return !address.isEmpty(); }
};

/// 设置存储。
class SettingsStore : public QObject {
  Q_OBJECT

 public:
  explicit SettingsStore(QObject* parent = nullptr);

  /// 读盘（构造后自动调用）。
  void load();

  /// 写盘。
  void save();

  /// 所有已保存的服务器。
  [[nodiscard]] const std::vector<ServerEntry>& entries() const { return entries_; }

  /// 当前激活的服务器（没有则返回 nullptr）。
  [[nodiscard]] const ServerEntry* activeEntry() const;

  /// 按地址找一条。
  [[nodiscard]] const ServerEntry* findByAddress(const QString& address) const;

  /// 加或更新一条记录。已存在同地址的会覆盖。
  void upsert(const ServerEntry& entry);

  /// 删一条。
  void remove(const QString& address);

  /// 设成激活项。
  void setActive(const QString& address);

  /// 存文件路径（测试里要看内容）。
  [[nodiscard]] QString filePath() const { return path_; }

 signals:
  /// 设置变了（界面要刷新）。
  void changed();

 private:
  QString path_;
  std::vector<ServerEntry> entries_;
};

}  // namespace agentlink
