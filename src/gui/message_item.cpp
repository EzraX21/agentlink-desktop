#include "message_item.hpp"

#include <QAction>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QFile>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>

#include "core/auth_api.hpp"

namespace agentlink {
namespace {

constexpr int kInlineMaxWidth = 320;  ///< 内联图片最大宽度（px），保持聊天流紧凑。

QString timeText(qint64 ms) {
  const QDateTime dt = QDateTime::fromMSecsSinceEpoch(ms);
  const QDateTime now = QDateTime::currentDateTime();
  if (dt.date() == now.date()) return dt.toString(QStringLiteral("HH:mm"));
  return dt.toString(QStringLiteral("MM-dd HH:mm"));
}

/// 能点一下的 QLabel：QLabel 显示 pixmap 时不发 linkActivated，
/// 所以用回调转发左键点击。不用 Q_OBJECT —— 放在匿名命名空间里 moc 会困惑。
class ClickableLabel : public QLabel {
 public:
  using QLabel::QLabel;

  void setOnClick(std::function<void()> onClick) { onClick_ = std::move(onClick); }

 protected:
  void mousePressEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton && onClick_) onClick_();
    QLabel::mousePressEvent(event);
  }

 private:
  std::function<void()> onClick_;
};

}  // namespace

MessageItem::MessageItem(AuthApi* api, std::function<QString()> token, QWidget* parent)
    : QWidget(parent), api_(api), token_(std::move(token)) {
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(8, 6, 8, 6);
  outer->setSpacing(3);

  auto* header = new QHBoxLayout();
  header->setSpacing(6);
  nameLabel_ = new QLabel(this);
  nameLabel_->setObjectName(QStringLiteral("senderName"));
  timeLabel_ = new QLabel(this);
  timeLabel_->setObjectName(QStringLiteral("senderTime"));
  statusLabel_ = new QLabel(this);
  statusLabel_->setObjectName(QStringLiteral("msgStatus"));
  header->addWidget(nameLabel_);
  header->addWidget(timeLabel_);
  header->addWidget(statusLabel_);
  header->addStretch(1);
  outer->addLayout(header);

  quoteLabel_ = new QLabel(this);
  quoteLabel_->setObjectName(QStringLiteral("replyQuote"));
  quoteLabel_->setWordWrap(true);
  quoteLabel_->setVisible(false);
  outer->addWidget(quoteLabel_);

  contentBox_ = new QWidget(this);
  outer->addWidget(contentBox_);

  streamingLabel_ = new QLabel(this);
  streamingLabel_->setObjectName(QStringLiteral("streamingText"));
  streamingLabel_->setWordWrap(true);
  streamingLabel_->setVisible(false);
  outer->addWidget(streamingLabel_);

  interruptButton_ = new QPushButton(QStringLiteral("打断"), this);
  interruptButton_->setObjectName(QStringLiteral("interruptButton"));
  interruptButton_->setToolTip(QStringLiteral("停止 AI 的流式回复"));
  interruptButton_->setVisible(false);
  outer->addWidget(interruptButton_, 0, Qt::AlignLeft);

  connect(interruptButton_, &QPushButton::clicked, this, [this]() {
    emit interruptRequested(messageId_);
  });
}

void MessageItem::setMessage(const QVariantMap& message, bool isSelf) {
  message_ = message;
  isSelf_ = isSelf;
  messageId_ = message.value(QStringLiteral("id")).toString();
  const QVariantMap sender = message.value(QStringLiteral("sender")).toMap();
  senderId_ = sender.value(QStringLiteral("id")).toString();
  rebuild();
}

void MessageItem::markRevoked() {
  message_.insert(QStringLiteral("revoked_at"),
                  static_cast<double>(QDateTime::currentMSecsSinceEpoch()));
  rebuild();
}

void MessageItem::updateStreaming(const QString& text, bool streaming) {
  streamingLabel_->setVisible(streaming);
  interruptButton_->setVisible(streaming);
  if (streaming) {
    streamingLabel_->setText(text.isEmpty() ? QStringLiteral("（正在输入…）") : text);
  }
}

void MessageItem::rebuild() {
  // 清空旧的内容块
  if (contentBox_->layout() != nullptr) {
    QLayoutItem* item;
    while ((item = contentBox_->layout()->takeAt(0)) != nullptr) {
      if (item->widget() != nullptr) item->widget()->deleteLater();
      delete item;
    }
  } else {
    new QVBoxLayout(contentBox_);
  }
  auto* contentLayout = contentBox_->layout();
  contentLayout->setContentsMargins(0, 0, 0, 0);
  contentLayout->setSpacing(4);

  const QVariantMap sender = message_.value(QStringLiteral("sender")).toMap();
  const QString senderName = sender.value(QStringLiteral("display_name")).toString();
  const qint64 createdAt = static_cast<qint64>(message_.value(QStringLiteral("created_at")).toDouble());
  const bool revoked = !message_.value(QStringLiteral("revoked_at")).isNull();
  const bool edited = !message_.value(QStringLiteral("edited_at")).isNull();
  const bool streaming = message_.value(QStringLiteral("state")).toString() == QStringLiteral("streaming");

  nameLabel_->setText(isSelf_ ? QStringLiteral("我") : (senderName.isEmpty() ? QStringLiteral("(未知)") : senderName));
  timeLabel_->setText(timeText(createdAt));

  QString status;
  if (revoked) status = QStringLiteral("（已撤回）");
  else if (edited) status = QStringLiteral("（已编辑）");
  statusLabel_->setText(status);
  statusLabel_->setVisible(!status.isEmpty());

  // 引用：reply_to 存在时，用解析器拿被引用消息的正文（拿不到就只显示"回复"）。
  const QString replyTo = message_.value(QStringLiteral("reply_to")).toString();
  if (!replyTo.isEmpty() && quoteResolver_) {
    const QString quoted = quoteResolver_(replyTo);
    quoteLabel_->setText(QStringLiteral("↩ 回复：%1").arg(quoted.isEmpty() ? QStringLiteral("…") : quoted));
    quoteLabel_->setVisible(true);
  } else {
    quoteLabel_->setVisible(false);
  }

  // 正文
  if (revoked) {
    auto* revokedLabel = new QLabel(QStringLiteral("这条消息已被撤回"), contentBox_);
    revokedLabel->setObjectName(QStringLiteral("revokedText"));
    contentLayout->addWidget(revokedLabel);
  } else {
    const QVariantList blocks = message_.value(QStringLiteral("content")).toList();
    for (const QVariant& b : blocks) {
      const QVariantMap block = b.toMap();
      const QString type = block.value(QStringLiteral("type")).toString();
      if (type == QStringLiteral("text")) {
        addTextBlock(block.value(QStringLiteral("text")).toString());
      } else if (type == QStringLiteral("image")) {
        addImageBlock(block.value(QStringLiteral("attachment_id")).toString());
      } else if (type == QStringLiteral("file")) {
        addFileBlock(block);
      }
    }
  }

  // 流式状态
  updateStreaming(QString(), false);
  if (streaming) {
    QString acc;
    const QVariantList blocks = message_.value(QStringLiteral("content")).toList();
    for (const QVariant& b : blocks) {
      const QVariantMap block = b.toMap();
      if (block.value(QStringLiteral("type")).toString() == QStringLiteral("text")) {
        acc += block.value(QStringLiteral("text")).toString();
      }
    }
    updateStreaming(acc, true);
  }
}

void MessageItem::addTextBlock(const QString& text) {
  auto* label = new QLabel(text, contentBox_);
  label->setWordWrap(true);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  contentBox_->layout()->addWidget(label);
}

void MessageItem::addImageBlock(const QString& attachmentId) {
  auto* label = new ClickableLabel(QStringLiteral("🖼 图片（加载中…）"), contentBox_);
  label->setObjectName(QStringLiteral("imageBlock"));
  label->setCursor(Qt::PointingHandCursor);
  label->setOnClick([this, attachmentId]() { emit imageClicked(attachmentId); });
  loadImageInto(label, attachmentId);
  contentBox_->layout()->addWidget(label);
}

void MessageItem::addFileBlock(const QVariantMap& block) {
  const QString attachmentId = block.value(QStringLiteral("attachment_id")).toString();
  const QString filename = block.value(QStringLiteral("filename")).toString();

  auto* label = new ClickableLabel(QStringLiteral("📎 %1").arg(filename.isEmpty() ? attachmentId : filename),
                                   contentBox_);
  label->setObjectName(QStringLiteral("fileBlock"));
  label->setCursor(Qt::PointingHandCursor);
  label->setToolTip(QStringLiteral("点击下载并打开"));
  label->setOnClick([this, attachmentId, filename]() { emit fileClicked(attachmentId, filename); });

  // 视频文件：尝试拉服务端抽的帧当预览；不是视频（404）就保持 📎。
  loadThumbnailInto(label, attachmentId, filename);

  contentBox_->layout()->addWidget(label);
}

void MessageItem::loadImageInto(QLabel* label, const QString& attachmentId) {
  const QString token = token_();
  if (api_ == nullptr || token.isEmpty()) {
    label->setText(QStringLiteral("🖼 图片"));
    return;
  }

  // 下载到临时目录 → 载入 QPixmap → 立刻删临时文件（pixmap 已经解进内存）。
  // 用固定临时目录而不是 QTemporaryDir：下载是异步的，临时目录要活到回调。
  const QString tmpDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
  const QString dest = tmpDir + QStringLiteral("/agentlink-img-") +
                       attachmentId.mid(0, 8) + QStringLiteral(".bin");

  api_->downloadAttachment(token, attachmentId, QStringLiteral("image"), dest,
                           [label, dest](bool ok, const QString&) {
                             // 回调是异步的：切会话后 label 可能已被销毁。
                             // 用 QPointer 自动置空，避免写野指针。
                             QPointer<QLabel> guard(label);
                             if (guard.isNull()) return;
                             QPixmap pix;
                             if (ok) {
                               pix.load(dest);
                               QFile::remove(dest);
                             }
                             if (pix.isNull()) {
                               guard->setText(QStringLiteral("🖼 图片"));
                             } else {
                               if (pix.width() > kInlineMaxWidth) {
                                 pix = pix.scaledToWidth(kInlineMaxWidth, Qt::SmoothTransformation);
                               }
                               guard->setPixmap(pix);
                             }
                           });
}

void MessageItem::loadThumbnailInto(QLabel* label, const QString& attachmentId, const QString& filename) {
  const QString token = token_();
  if (api_ == nullptr || token.isEmpty()) return;

  api_->fetchThumbnail(token, attachmentId, [label, filename](bool ok, const QByteArray& jpeg, const QString&) {
    QPointer<QLabel> guard(label);
    if (guard.isNull() || !ok) return;
    QPixmap pix;
    if (!pix.loadFromData(jpeg)) return;
    if (pix.width() > kInlineMaxWidth) {
      pix = pix.scaledToWidth(kInlineMaxWidth, Qt::SmoothTransformation);
    }
    guard->setPixmap(pix);
    guard->setToolTip(filename.isEmpty() ? QStringLiteral("视频") : filename);
  });
}

void MessageItem::contextMenuEvent(QContextMenuEvent* event) {
  QMenu menu(this);

  QAction* replyAction = menu.addAction(QStringLiteral("回复"));
  QAction* copyAction = menu.addAction(QStringLiteral("复制"));
  QAction* revokeAction = nullptr;
  if (isSelf_) {
    menu.addSeparator();
    revokeAction = menu.addAction(QStringLiteral("撤回"));
  }

  QAction* chosen = menu.exec(event->globalPos());
  if (chosen == replyAction) {
    emit replyRequested(messageId_);
  } else if (chosen == copyAction) {
    QString text;
    const QVariantList blocks = message_.value(QStringLiteral("content")).toList();
    for (const QVariant& b : blocks) {
      const QVariantMap block = b.toMap();
      if (block.value(QStringLiteral("type")).toString() == QStringLiteral("text")) {
        text += block.value(QStringLiteral("text")).toString();
      }
    }
    if (!text.isEmpty()) {
      QGuiApplication::clipboard()->setText(text);
    }
  } else if (revokeAction != nullptr && chosen == revokeAction) {
    emit revokeRequested(messageId_);
  }
}

}  // namespace agentlink
