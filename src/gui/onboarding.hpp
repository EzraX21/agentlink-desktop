#pragma once

#include <QDialog>
#include <QWidget>

class QLineEdit;
class QLabel;
class QPushButton;
class QStackedWidget;
class QComboBox;

namespace agentlink {

class AuthApi;
class Client;
class SettingsStore;

/**
 * 引导流程（参考 Element 的做法）。
 *
 * 三步：
 *   1. **服务器地址** —— 用户手填（`chat.example.com` / `https://…` 都认）
 *   2. **注册** —— 账号 + 昵称 + 密码（服务端要求密码 ≥ 8 位）
 *   3. **登录** —— 已有账号直接登录
 *
 * 走完第 1 步之后地址就记住了；以后启动直接进聊天，
 * 只在 token 失效时才回到这一步。
 */
class Onboarding : public QDialog {
  Q_OBJECT

 public:
  Onboarding(AuthApi* api, SettingsStore* settings, QWidget* parent = nullptr);

  /// 引导结束后拿到的登录态（access token 与用户 id）。
  [[nodiscard]] QString accessToken() const { return accessToken_; }
  [[nodiscard]] QString userId() const { return userId_; }
  [[nodiscard]] QString displayName() const { return displayName_; }
  /// 规范化后的服务端地址。
  [[nodiscard]] QString serverAddress() const { return serverAddress_; }

 private slots:
  void onAddressNext();
  void onRegister();
  void onLogin();
  void onBack();

 private:
  void showPage(int index);
  void showError(const QString& message);
  void succeed(const QString& token, const QString& refreshToken, const QString& userId,
               const QString& displayName, const QString& username);

  AuthApi* api_;
  SettingsStore* settings_;

  QStackedWidget* stack_ = nullptr;

  // 第 1 页：地址
  QLineEdit* addressEdit_ = nullptr;
  // 第 2 页：注册
  QLineEdit* regUsername_ = nullptr;
  QLineEdit* regDisplayName_ = nullptr;
  QLineEdit* regPassword_ = nullptr;
  // 第 3 页：登录
  QLineEdit* loginUsername_ = nullptr;
  QLineEdit* loginPassword_ = nullptr;

  QLabel* errorLabel_ = nullptr;
  QLabel* titleLabel_ = nullptr;
  QPushButton* backButton_ = nullptr;
  QPushButton* nextButton_ = nullptr;

  QString serverAddress_;
  QString accessToken_;
  QString userId_;
  QString displayName_;
};

}  // namespace agentlink
