#include "message_item.hpp"

#include <QAction>
#include <QBoxLayout>
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

/// 预设表情（右键「回应」菜单与「＋」按钮共用的那一排）。
const char* const kReactionEmojis[] = {"👍", "❤️", "😂", "😮", "😢"};

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

  reactionsBox_ = new QWidget(this);
  outer->addWidget(reactionsBox_);

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
  reactions_ = message.value(QStringLiteral("reactions")).toList();
  rebuild();
}

void MessageItem::markRevoked() {
  message_.insert(QStringLiteral("revoked_at"),
                  static_cast<double>(QDateTime::currentMSecsSinceEpoch()));
  rebuild();
}

void MessageItem::setReactions(const QVariantList& reactions) {
  reactions_ = reactions;
  rebuildReactions();
}

bool MessageItem::mineReaction(const QString& emoji) const {
  for (const QVariant& r : reactions_) {
    const QVariantMap reaction = r.toMap();
    if (reaction.value(QStringLiteral("emoji")).toString() == emoji) {
      const QVariantList userIds = reaction.value(QStringLiteral("user_ids")).toList();
      for (const QVariant& id : userIds) {
        if (id.toString() == selfId_) return true;
      }
    }
  }
  return false;
}

void MessageItem::rebuildReactions() {
  // 清空旧的回应条
  if (reactionsBox_->layout() != nullptr) {
    QLayoutItem* item;
    while ((item = reactionsBox_->layout()->takeAt(0)) != nullptr) {
      if (item->widget() != nullptr) item->widget()->deleteLater();
      delete item;
    }
  } else {
    new QHBoxLayout(reactionsBox_);
  }
  auto* layout = static_cast<QBoxLayout*>(reactionsBox_->layout());
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(4);

  bool any = false;
  for (const QVariant& r : reactions_) {
    const QVariantMap reaction = r.toMap();
    const QString emoji = reaction.value(QStringLiteral("emoji")).toString();
    const int count = reaction.value(QStringLiteral("user_ids")).toList().size();
    if (emoji.isEmpty()) continue;

    const bool mine = mineReaction(emoji);
    auto* chip = new QPushButton(
        QStringLiteral("%1 %2").arg(emoji).arg(count), reactionsBox_);
    chip->setCursor(Qt::PointingHandCursor);
    chip->setToolTip(mine ? QStringLiteral("点击取消") : QStringLiteral("点击回应"));
    chip->setStyleSheet(mine ? QStringLiteral("background:#dbe7ff;") : QString());
    connect(chip, &QPushButton::clicked, this, [this, emoji, mine]() {
      emit reactionRequested(messageId_, emoji, mine);
    });
    layout->addWidget(chip);
    any = true;
  }
  Q_UNUSED(any)

  // 「＋」按钮：加一个预设表情（没有回应时也显示，让用户能回应）
  auto* addButton = new QPushButton(QStringLiteral("＋"), reactionsBox_);
  addButton->setFixedWidth(28);
  addButton->setToolTip(QStringLiteral("回应"));
  connect(addButton, &QPushButton::clicked, this, [this, addButton]() {
    QMenu menu(this);
    for (const char* e : kReactionEmojis) {
      const QString emoji = QString::fromUtf8(e);
      QAction* action = menu.addAction(emoji);
      QObject::connect(action, &QAction::triggered, this, [this, emoji]() {
        emit reactionRequested(messageId_, emoji, false);
      });
    }
    menu.exec(addButton->mapToGlobal(QPoint(0, addButton->height())));
  });
  layout->addWidget(addButton);
  layout->addStretch(1);

  reactionsBox_->setVisible(true);
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

  const QString replyTo = message_.value(QStringLiteral("reply_to")).toString();
  if (!replyTo.isEmpty() && quoteResolver_) {
    const QString quoted = quoteResolver_(replyTo);
    quoteLabel_->setText(QStringLiteral("↩ 回复：%1").arg(quoted.isEmpty() ? QStringLiteral("…") : quoted));
    quoteLabel_->setVisible(true);
  } else {
    quoteLabel_->setVisible(false);
  }

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

  // 表情回应
  reactions_ = message_.value(QStringLiteral("reactions")).toList();
  rebuildReactions();

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

  loadThumbnailInto(label, attachmentId, filename);

  contentBox_->layout()->addWidget(label);
}

void MessageItem::loadImageInto(QLabel* label, const QString& attachmentId) {
  const QString token = token_();
  if (api_ == nullptr || token.isEmpty()) {
    label->setText(QStringLiteral("🖼 图片"));
    return;
  }

  const QString tmpDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
  const QString dest = tmpDir + QStringLiteral("/agentlink-img-") +
                       attachmentId.mid(0, 8) + QStringLiteral(".bin");

  api_->downloadAttachment(token, attachmentId, QStringLiteral("image"), dest,
                           [label, dest](bool ok, const QString&) {
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

  // 回应子菜单
  QMenu* reactMenu = menu.addMenu(QStringLiteral("回应"));
  for (const char* e : kReactionEmojis) {
    const QString emoji = QString::fromUtf8(e);
    QAction* action = reactMenu->addAction(emoji);
    QObject::connect(action, &QAction::triggered, this, [this, emoji]() {
      emit reactionRequested(messageId_, emoji, mineReaction(emoji));
    });
  }

  QAction* editAction = nullptr;
  QAction* forwardAction = nullptr;
  QAction* pinAction = nullptr;
  if (!message_.value(QStringLiteral("revoked_at")).isNull()) {
    // 已撤回：除了复制什么都别做
  } else {
    if (isSelf_) {
      menu.addSeparator();
      editAction = menu.addAction(QStringLiteral("编辑"));
    }
    forwardAction = menu.addAction(QStringLiteral("转发"));
    if (canPin_) {
      pinAction = menu.addAction(QStringLiteral("置顶"));
    }
  }

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
  } else if (editAction != nullptr && chosen == editAction) {
    emit editRequested(messageId_);
  } else if (forwardAction != nullptr && chosen == forwardAction) {
    emit forwardRequested(messageId_);
  } else if (pinAction != nullptr && chosen == pinAction) {
    emit pinRequested(messageId_);
  } else if (revokeAction != nullptr && chosen == revokeAction) {
    emit revokeRequested(messageId_);
  }
}

}  // namespace agentlink
