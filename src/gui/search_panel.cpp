#include "search_panel.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/client.hpp"

namespace agentlink {

SearchPanel::SearchPanel(Client* client, QWidget* parent)
    : QWidget(parent), client_(client) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(8, 8, 8, 8);

  auto* topRow = new QHBoxLayout();
  input_ = new QLineEdit(this);
  input_->setPlaceholderText(QStringLiteral("搜索消息…"));
  searchButton_ = new QPushButton(QStringLiteral("搜索"), this);
  topRow->addWidget(input_, 1);
  topRow->addWidget(searchButton_);
  layout->addLayout(topRow);

  auto* hint = new QLabel(QStringLiteral("按关键词搜全部会话的消息，点结果跳转。"), this);
  hint->setStyleSheet(QStringLiteral("color:#888;"));
  layout->addWidget(hint);

  results_ = new QListWidget(this);
  layout->addWidget(results_, 1);

  connect(searchButton_, &QPushButton::clicked, this, &SearchPanel::onSearch);
  connect(input_, &QLineEdit::returnPressed, this, &SearchPanel::onSearch);
  connect(results_, &QListWidget::itemActivated, this, &SearchPanel::onResultActivated);
}

void SearchPanel::onSearch() {
  const QString q = input_->text().trimmed();
  if (q.isEmpty() || client_ == nullptr) return;

  results_->clear();
  hits_.clear();
  results_->addItem(QStringLiteral("搜索中…"));

  client_->send(QStringLiteral("search.messages"),
                QVariantMap{{QStringLiteral("q"), q}, {QStringLiteral("limit"), 50}},
                [this](bool ok, const QVariantMap& data, const WireError& error) {
                  if (!ok) {
                    results_->clear();
                    results_->addItem(QStringLiteral("搜索失败：%1").arg(error.message));
                    return;
                  }
                  hits_ = data.value(QStringLiteral("hits")).toList();
                  results_->clear();
                  if (hits_.isEmpty()) {
                    results_->addItem(QStringLiteral("没有结果"));
                    return;
                  }
                  for (const QVariant& h : hits_) {
                    const QVariantMap hit = h.toMap();
                    const QString excerpt = hit.value(QStringLiteral("excerpt")).toString();
                    const QString convId = hit.value(QStringLiteral("conv_id")).toString();
                    results_->addItem(QStringLiteral("[%1] %2").arg(convId.mid(0, 8), excerpt));
                  }
                });
}

void SearchPanel::onResultActivated(QListWidgetItem* item) {
  const int row = results_->row(item);
  if (row < 0 || row >= hits_.size()) return;
  const QVariantMap hit = hits_.at(row).toMap();
  emit resultClicked(hit.value(QStringLiteral("conv_id")).toString(),
                     hit.value(QStringLiteral("message_id")).toString(),
                     static_cast<qint64>(hit.value(QStringLiteral("seq")).toDouble()));
}

}  // namespace agentlink
