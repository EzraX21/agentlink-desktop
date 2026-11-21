#pragma once

#include <QWidget>
#include <QVariantMap>

class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;

namespace agentlink {

class Client;

/**
 * 消息搜索面板：输入关键词 → search.messages → 点结果跳转。
 *
 * 全局搜索（不限定会话），返回的每条结果带 conv_id / message_id / seq /
 * excerpt，点一下跳到那条消息所在的会话。
 */
class SearchPanel : public QWidget {
  Q_OBJECT

 public:
  explicit SearchPanel(Client* client, QWidget* parent = nullptr);

 signals:
  /// 点了某条结果：上层切会话并把历史拉到那条消息。
  void resultClicked(const QString& convId, const QString& messageId, qint64 seq);
  /// 关闭面板。
  void closeRequested();

 private slots:
  void onSearch();
  void onResultActivated(QListWidgetItem* item);

 private:
  Client* client_ = nullptr;
  QLineEdit* input_ = nullptr;
  QPushButton* searchButton_ = nullptr;
  QListWidget* results_ = nullptr;
  QVariantList hits_;
};

}  // namespace agentlink
