#include "conversation_list.hpp"

#include <QListWidget>
#include <QVBoxLayout>

#include "core/client.hpp"

namespace agentlink {

ConversationList::ConversationList(Client* client, QWidget* parent)
    : QWidget(parent), client_(client) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  list_ = new QListWidget(this);
  list_->setObjectName(QStringLiteral("conversationList"));
  layout->addWidget(list_);

  connect(list_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* current) {
    if (current == nullptr) return;
    const QString convId = current->data(Qt::UserRole).toString();
    if (selectionHandler_) selectionHandler_(convId);
    emit selectionChanged(convId);
  });
}

void ConversationList::setConversations(const QVariantList& conversations) {
  const QString previous = selectedConversationId();
  list_->clear();

  for (const QVariant& v : conversations) {
    const QVariantMap conv = v.toMap();
    const QString id = conv.value(QStringLiteral("id")).toString();
    const QString title = conv.value(QStringLiteral("title")).toString();
    const int memberCount = conv.value(QStringLiteral("member_count")).toInt();

    QString label = title;
    if (memberCount > 0) {
      label = QStringLiteral("%1  (%2)").arg(title).arg(memberCount);
    }

    auto* item = new QListWidgetItem(label, list_);
    item->setData(Qt::UserRole, id);
    item->setToolTip(conv.value(QStringLiteral("announcement")).toString());
  }

  if (!previous.isEmpty()) {
    selectConversation(previous);
  } else if (list_->count() > 0) {
    list_->setCurrentRow(0);
  }
}

QString ConversationList::selectedConversationId() const {
  QListWidgetItem* item = list_->currentItem();
  return item == nullptr ? QString() : item->data(Qt::UserRole).toString();
}

void ConversationList::selectConversation(const QString& convId) {
  QListWidgetItem* item = itemFor(convId);
  if (item != nullptr) {
    list_->setCurrentItem(item);
  }
}

QListWidgetItem* ConversationList::itemFor(const QString& convId) const {
  for (int i = 0; i < list_->count(); ++i) {
    QListWidgetItem* item = list_->item(i);
    if (item->data(Qt::UserRole).toString() == convId) return item;
  }
  return nullptr;
}

void ConversationList::setUnread(const QString& convId, int count) {
  QListWidgetItem* item = itemFor(convId);
  if (item == nullptr) return;
  if (count <= 0) {
    // 去掉角标：QListWidgetItem 没有"角标"概念，用文本前缀表达。
    QString text = item->text();
    const int marker = text.indexOf(QStringLiteral(" ●"));
    if (marker >= 0) item->setText(text.left(marker));
    return;
  }
  QString text = item->text();
  if (!text.contains(QStringLiteral(" ●"))) {
    item->setText(text + QStringLiteral(" ●"));
  }
}

}  // namespace agentlink
