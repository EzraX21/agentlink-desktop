#include "onboarding.hpp"

#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "core/auth_api.hpp"
#include "core/client.hpp"
#include "core/settings.hpp"

namespace agentlink {
namespace {

/// 页面样式：留白 + 一个标题 + 表单。
QWidget* makePage(const QString& hint, QWidget** formHost) {
  auto* page = new QWidget;
  auto* layout = new QVBoxLayout(page);
  layout->setContentsMargins(32, 32, 32, 32);
  layout->setSpacing(16);

  auto* hintLabel = new QLabel(hint);
  hintLabel->setWordWrap(true);
  hintLabel->setObjectName(QStringLiteral("hintLabel"));
  layout->addWidget(hintLabel);

  auto* form = new QWidget;
  auto* formLayout = new QFormLayout(form);
  formLayout->setContentsMargins(0, 0, 0, 0);
  formLayout->setSpacing(10);
  layout->addWidget(form);

  *formHost = form;
  return page;
}

}  // namespace

Onboarding::Onboarding(AuthApi* api, SettingsStore* settings, QWidget* parent)
    : QDialog(parent), api_(api), settings_(settings) {
  setWindowTitle(QStringLiteral("连接 AgentLink"));
  setMinimumWidth(440);

  stack_ = new QStackedWidget(this);

  // ---- 第 1 页：服务器地址 ----
  {
    QWidget* form = nullptr;
    QWidget* page = makePage(
        QStringLiteral(
            "输入你的 AgentLink 服务器地址。\n"
            "可以写 chat.example.com，也可以写完整的 https://chat.example.com。"),
        &form);
    auto* formLayout = qobject_cast<QFormLayout*>(form->layout());
    addressEdit_ = new QLineEdit;
    addressEdit_->setPlaceholderText(QStringLiteral("例如 chat.example.com"));
    formLayout->addRow(QStringLiteral("服务器地址"), addressEdit_);
    stack_->addWidget(page);
  }

  // ---- 第 2 页：注册 ----
  {
    QWidget* form = nullptr;
    QWidget* page =
        makePage(QStringLiteral("在这个服务器上创建一个账号。密码至少 8 位。"), &form);
    auto* formLayout = qobject_cast<QFormLayout*>(form->layout());
    regUsername_ = new QLineEdit;
    regUsername_->setPlaceholderText(QStringLiteral("字母数字与 _ . -"));
    regDisplayName_ = new QLineEdit;
    regDisplayName_->setPlaceholderText(QStringLiteral("别人看到的名字"));
    regPassword_ = new QLineEdit;
    regPassword_->setEchoMode(QLineEdit::Password);
    formLayout->addRow(QStringLiteral("用户名"), regUsername_);
    formLayout->addRow(QStringLiteral("昵称"), regDisplayName_);
    formLayout->addRow(QStringLiteral("密码"), regPassword_);
    stack_->addWidget(page);
  }

  // ---- 第 3 页：登录 ----
  {
    QWidget* form = nullptr;
    QWidget* page = makePage(QStringLiteral("已经有账号了？直接登录。"), &form);
    auto* formLayout = qobject_cast<QFormLayout*>(form->layout());
    loginUsername_ = new QLineEdit;
    loginPassword_ = new QLineEdit;
    loginPassword_->setEchoMode(QLineEdit::Password);
    formLayout->addRow(QStringLiteral("用户名"), loginUsername_);
    formLayout->addRow(QStringLiteral("密码"), loginPassword_);
    stack_->addWidget(page);
  }

  titleLabel_ = new QLabel(this);
  errorLabel_ = new QLabel(this);
  errorLabel_->setObjectName(QStringLiteral("errorLabel"));
  errorLabel_->setWordWrap(true);
  errorLabel_->setVisible(false);

  backButton_ = new QPushButton(QStringLiteral("返回"), this);
  nextButton_ = new QPushButton(QStringLiteral("下一步"), this);
  nextButton_->setDefault(true);

  auto* buttons = new QWidget(this);
  auto* buttonLayout = new QHBoxLayout(buttons);
  buttonLayout->setContentsMargins(32, 0, 32, 24);
  buttonLayout->addWidget(backButton_);
  buttonLayout->addStretch(1);
  buttonLayout->addWidget(nextButton_);

  auto* root = new QVBoxLayout(this);
  root->addWidget(stack_);
  root->addWidget(errorLabel_);
  root->addWidget(buttons);

  connect(nextButton_, &QPushButton::clicked, this, [this] {
    switch (stack_->currentIndex()) {
      case 0:
        onAddressNext();
        break;
      case 1:
        onRegister();
        break;
      case 2:
        onLogin();
        break;
      default:
        break;
    }
  });
  connect(backButton_, &QPushButton::clicked, this, &Onboarding::onBack);

  showPage(0);
}

void Onboarding::showPage(int index) {
  stack_->setCurrentIndex(index);
  errorLabel_->setVisible(false);
  switch (index) {
    case 0:
      nextButton_->setText(QStringLiteral("下一步"));
      backButton_->setVisible(false);
      addressEdit_->setFocus();
      break;
    case 1:
      nextButton_->setText(QStringLiteral("注册"));
      backButton_->setVisible(true);
      regUsername_->setFocus();
      break;
    case 2:
      nextButton_->setText(QStringLiteral("登录"));
      backButton_->setVisible(true);
      loginUsername_->setFocus();
      break;
    default:
      break;
  }
}

void Onboarding::showError(const QString& message) {
  errorLabel_->setText(message);
  errorLabel_->setVisible(true);
}

void Onboarding::onAddressNext() {
  const QString input = addressEdit_->text().trimmed();
  if (input.isEmpty()) {
    showError(QStringLiteral("请填写服务器地址"));
    return;
  }
  const auto address = ServerAddress::parse(input);
  if (!address.has_value()) {
    showError(QStringLiteral("这个地址看不懂。试试 chat.example.com 或 https://chat.example.com"));
    return;
  }
  serverAddress_ = address->url;
  api_->setHttpBase(address->httpBase());

  // 第 2 页：注册；第 3 页：登录。用一个小切换按钮在两者间切。
  showPage(1);
}

void Onboarding::onBack() {
  showError(QString());
  if (stack_->currentIndex() == 1) {
    showPage(0);
  } else if (stack_->currentIndex() == 2) {
    showPage(1);
  }
}

void Onboarding::onRegister() {
  const QString username = regUsername_->text().trimmed();
  const QString password = regPassword_->text();
  if (username.isEmpty() || password.isEmpty()) {
    showError(QStringLiteral("用户名和密码都要填"));
    return;
  }
  if (password.length() < 8) {
    showError(QStringLiteral("密码至少 8 位"));
    return;
  }

  nextButton_->setEnabled(false);
  api_->registerUser(username, regDisplayName_->text().trimmed(), password,
                     [this, username](const AuthResult& result) {
                       nextButton_->setEnabled(true);
                       if (!result.ok) {
                         showError(result.error);
                         return;
                       }
                       succeed(result.accessToken, result.refreshToken, result.userId,
                               result.displayName, username);
                     });
}

void Onboarding::onLogin() {
  const QString username = loginUsername_->text().trimmed();
  const QString password = loginPassword_->text();
  if (username.isEmpty() || password.isEmpty()) {
    showError(QStringLiteral("用户名和密码都要填"));
    return;
  }

  nextButton_->setEnabled(false);
  api_->login(username, password, [this, username](const AuthResult& result) {
    nextButton_->setEnabled(true);
    if (!result.ok) {
      showError(result.error);
      return;
    }
    succeed(result.accessToken, result.refreshToken, result.userId, result.displayName, username);
  });
}

void Onboarding::succeed(const QString& token, const QString& refreshToken, const QString& userId,
                          const QString& displayName, const QString& username) {
  accessToken_ = token;
  userId_ = userId;
  displayName_ = displayName;

  // 记住这台服务器：下次启动直接进聊天。
  ServerEntry entry;
  entry.address = serverAddress_;
  entry.rawAddress = serverAddress_;
  entry.username = username;
  entry.displayName = displayName;
  entry.accessToken = token;
  entry.refreshToken = refreshToken;
  entry.active = true;
  settings_->upsert(entry);
  settings_->setActive(serverAddress_);

  accept();
}

}  // namespace agentlink
