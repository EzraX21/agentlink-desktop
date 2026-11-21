#pragma once

#include <QWidget>
#include <QVariantMap>
#include <functional>

class QLabel;
class QPushButton;

namespace agentlink {

class AuthApi;

/**
 * 一条消息的 widget（聊天流里的最小单元）。
 *
 * ## 为什么每条消息一个 QWidget 而不是继续用 QTextBrowser
 *
 * QTextBrowser 是**一整块富文本文档**，没法对"某一条消息"做：
 *   - 右键菜单（回复 / 撤回 / 复制）
 *   - 流式正文的**原地更新**（stream.delta 来了要改一条已有的消息）
 *   - 图片内联缩略图（一个 <img> 标签加载远程 URL 要自己管认证头，浏览器控件不干这个）
 *
 * 换成 QListWidget + 每行一个 MessageItem，上面这些都变成"改一个 widget"。
 * 代价是自己算行高 —— 但 QListWidget 的 item 是自适应的，代价可控。
 */
class MessageItem : public QWidget {
  Q_OBJECT

 public:
  MessageItem(AuthApi* api, std::function<QString()> token, QWidget* parent = nullptr);

  /// 整条消息重渲染（首次插入、编辑、撤回时用）。
  void setMessage(const QVariantMap& message, bool isSelf);

  /// 撤回：原地标记为"已撤回"。服务端 history.fetch 会过滤掉撤回的消息，
  /// 所以只能本地标记（重拉历史会把这条消息直接弄丢）。
  void markRevoked();

  /// 流式更新：只改正文与「打断」按钮，不重建整个 widget。
  void updateStreaming(const QString& text, bool streaming);

  /// 引用消息的正文解析器（历史里的 reply_to 靠它拿到"回复了谁/什么"）。
  void setQuoteResolver(std::function<QString(const QString&)> resolver) {
    quoteResolver_ = std::move(resolver);
  }

  [[nodiscard]] QString messageId() const { return messageId_; }
  [[nodiscard]] QString senderId() const { return senderId_; }

 signals:
  void replyRequested(const QString& messageId);
  void revokeRequested(const QString& messageId);
  void interruptRequested(const QString& messageId);
  /// 点图片：打开大图。
  void imageClicked(const QString& attachmentId);
  /// 点文件：下载并交给系统打开。
  void fileClicked(const QString& attachmentId, const QString& filename);

 protected:
  void contextMenuEvent(QContextMenuEvent* event) override;

 private:
  void rebuild();
  void addTextBlock(const QString& text);
  void addImageBlock(const QString& attachmentId);
  void addFileBlock(const QVariantMap& block);
  void loadImageInto(QLabel* label, const QString& attachmentId);
  void loadThumbnailInto(QLabel* label, const QString& attachmentId, const QString& fallback);
  void openImageFull(const QString& attachmentId);

  AuthApi* api_ = nullptr;
  std::function<QString()> token_;
  std::function<QString(const QString&)> quoteResolver_;

  QLabel* nameLabel_ = nullptr;
  QLabel* timeLabel_ = nullptr;
  QLabel* statusLabel_ = nullptr;
  QLabel* quoteLabel_ = nullptr;
  QWidget* contentBox_ = nullptr;
  QLabel* streamingLabel_ = nullptr;
  QPushButton* interruptButton_ = nullptr;

  QVariantMap message_;
  QString messageId_;
  QString senderId_;
  bool isSelf_ = false;
};

}  // namespace agentlink
