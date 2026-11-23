#include "members_dialog.hpp"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/client.hpp"

namespace agentlink {
namespace {

QString roleText(const QString& role) {
  if (role == QStringLiteral("owner")) return QStringLiteral("群主");
  if (role == QStringLiteral("admin")) return QStringLiteral("管理员");
  return QStringLiteral("成员");
}

}  // namespace

MembersDialog::MembersDialog(Client* client, const QString& convId, const QString& selfId,
                             QVariantList members, QWidget* parent)
    : QDialog(parent),
      client_(client),
      convId_(convId),
      selfId_(selfId),
      members_(std::move(members)) {
  setWindowTitle(QStringLiteral("成员管理"));
  setModal(true);
  resize(520, 460);

  auto* layout = new QVBoxLayout(this);

  auto* hint = new QLabel(
      QStringLiteral("踢人 / 禁言 / 改角色都要求你是群主或管理员。选中一个成员再点操作。"),
      this);
  hint->setWordWrap(true);
  hint->setStyleSheet(QStringLiteral("color:#888;"));
  layout->addWidget(hint);

  list_ = new QListWidget(this);
  layout->addWidget(list_, 1);

  auto* buttons = new QHBoxLayout();
  inviteButton_ = new QPushButton(QStringLiteral("邀请…"), this);
  kickButton_ = new QPushButton(QStringLiteral("移出群"), this);
  muteButton_ = new QPushButton(QStringLiteral("禁言 10 分钟"), this);
  unmuteButton_ = new QPushButton(QStringLiteral("解除禁言"), this);
  promoteButton_ = new QPushButton(QStringLiteral("设为管理员"), this);
  demoteButton_ = new QPushButton(QStringLiteral("降为成员"), this);

  buttons->addWidget(inviteButton_);
  buttons->addWidget(kickButton_);
  buttons->addWidget(muteButton_);
  buttons->addWidget(unmuteButton_);
  buttons->addWidget(promoteButton_);
  buttons->addWidget(demoteButton_);
  layout->addLayout(buttons);

  auto* closeBox = new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(closeBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(closeBox);

  connect(inviteButton_, &QPushButton::clicked, this, &MembersDialog::onInvite);
  connect(kickButton_, &QPushButton::clicked, this, &MembersDialog::onKick);
  connect(muteButton_, &QPushButton::clicked, this, &MembersDialog::onMute);
  connect(unmuteButton_, &QPushButton::clicked, this, &MembersDialog::onUnmute);
  connect(promoteButton_, &QPushButton::clicked, this, &MembersDialog::onPromote);
  connect(demoteButton_, &QPushButton::clicked, this, &MembersDialog::onDemote);

  refresh();
}

void MembersDialog::refresh() {
  list_->clear();
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  for (const QVariant& m : members_) {
    const QVariantMap member = m.toMap();
    const QVariantMap user = member.value(QStringLiteral("user")).toMap();
    const QString userId = member.value(QStringLiteral("user_id")).toString();
    const QString name = user.value(QStringLiteral("display_name")).toString();
    const QString role = member.value(QStringLiteral("role")).toString();
    const bool isAgent = member.value(QStringLiteral("is_agent")).toBool();
    const qint64 mutedUntil = static_cast<qint64>(member.value(QStringLiteral("muted_until")).toDouble());

    QString line = QStringLiteral("%1  [%2]").arg(name.isEmpty() ? userId : name, roleText(role));
    if (userId == selfId_) line += QStringLiteral("  ← 你");
    if (isAgent) line += QStringLiteral("  · agent");
    if (mutedUntil > now) {
      const qint64 mins = (mutedUntil - now) / 60000 + 1;
      line += QStringLiteral("  · 禁言中（%1 分钟）").arg(mins);
    }

    auto* item = new QListWidgetItem(line, list_);
    item->setData(Qt::UserRole, userId);
    item->setData(Qt::UserRole + 1, role);
  }
}

QString MembersDialog::selectedUserId() const {
  const auto* item = list_->currentItem();
  return item == nullptr ? QString() : item->data(Qt::UserRole).toString();
}

QString MembersDialog::selectedRole() const {
  const auto* item = list_->currentItem();
  return item == nullptr ? QString() : item->data(Qt::UserRole + 1).toString();
}

void MembersDialog::callMemberOp(const QString& op, const QVariantMap& data, const QString& what) {
  client_->send(op, data, [this, what](bool ok, const QVariantMap&, const WireError& error) {
    if (!ok) {
      QMessageBox::warning(this, QStringLiteral("操作失败"),
                           QStringLiteral("%1失败：%2").arg(what, error.message));
      return;
    }
    // 成功后重新拉成员列表（服务端会推 member.update，这里是本地即时刷新）
    client_->send(QStringLiteral("conv.get"), QVariantMap{{QStringLiteral("conv_id"), convId_}},
                  [this](bool ok2, const QVariantMap& res, const WireError&) {
                    if (!ok2) return;
                    members_ = res.value(QStringLiteral("members")).toList();
                    refresh();
                  });
  });
}

void MembersDialog::onInvite() {
  bool ok = false;
  const QString raw = QInputDialog::getText(
      this, QStringLiteral("邀请成员"), QStringLiteral("用户 ID（逗号分隔）"),
      QLineEdit::Normal, QString(), &ok);
  if (!ok || raw.trimmed().isEmpty()) return;

  QStringList ids;
  for (const QString& part : raw.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
    const QString id = part.trimmed();
    if (!id.isEmpty()) ids << id;
  }
  if (ids.isEmpty()) return;

  callMemberOp(QStringLiteral("member.invite"),
               QVariantMap{{QStringLiteral("conv_id"), convId_},
                           {QStringLiteral("user_ids"), ids}},
               QStringLiteral("邀请"));
}

void MembersDialog::onKick() {
  const QString userId = selectedUserId();
  if (userId.isEmpty()) {
    QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("先选中一个成员。"));
    return;
  }
  if (userId == selfId_) {
    QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("不能移出自己。"));
    return;
  }
  if (selectedRole() == QStringLiteral("owner")) {
    QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("群主不能被移出。"));
    return;
  }
  if (QMessageBox::question(this, QStringLiteral("确认"),
                            QStringLiteral("确定把这个成员移出群吗？")) != QMessageBox::Yes) {
    return;
  }
  callMemberOp(QStringLiteral("member.kick"),
               QVariantMap{{QStringLiteral("conv_id"), convId_}, {QStringLiteral("user_id"), userId}},
               QStringLiteral("移出群"));
}

void MembersDialog::onMute() {
  const QString userId = selectedUserId();
  if (userId.isEmpty()) {
    QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("先选中一个成员。"));
    return;
  }
  const qint64 until = QDateTime::currentMSecsSinceEpoch() + 10 * 60 * 1000;
  callMemberOp(QStringLiteral("member.mute"),
               QVariantMap{{QStringLiteral("conv_id"), convId_},
                           {QStringLiteral("user_id"), userId},
                           {QStringLiteral("until_ms"), static_cast<double>(until)}},
               QStringLiteral("禁言"));
}

void MembersDialog::onUnmute() {
  const QString userId = selectedUserId();
  if (userId.isEmpty()) {
    QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("先选中一个成员。"));
    return;
  }
  // until_ms = 0 就是解除禁言（协议里约定的）
  callMemberOp(QStringLiteral("member.mute"),
               QVariantMap{{QStringLiteral("conv_id"), convId_},
                           {QStringLiteral("user_id"), userId},
                           {QStringLiteral("until_ms"), 0}},
               QStringLiteral("解除禁言"));
}

void MembersDialog::onPromote() {
  const QString userId = selectedUserId();
  if (userId.isEmpty()) {
    QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("先选中一个成员。"));
    return;
  }
  callMemberOp(QStringLiteral("member.role.set"),
               QVariantMap{{QStringLiteral("conv_id"), convId_},
                           {QStringLiteral("user_id"), userId},
                           {QStringLiteral("role"), QStringLiteral("admin")}},
               QStringLiteral("设为管理员"));
}

void MembersDialog::onDemote() {
  const QString userId = selectedUserId();
  if (userId.isEmpty()) {
    QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("先选中一个成员。"));
    return;
  }
  if (selectedRole() == QStringLiteral("owner")) {
    QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("群主不能被降级。"));
    return;
  }
  callMemberOp(QStringLiteral("member.role.set"),
               QVariantMap{{QStringLiteral("conv_id"), convId_},
                           {QStringLiteral("user_id"), userId},
                           {QStringLiteral("role"), QStringLiteral("member")}},
               QStringLiteral("降为成员"));
}

}  // namespace agentlink
