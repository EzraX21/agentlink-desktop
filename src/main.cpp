#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QPixmap>
#include <QTimer>
#include <cstdio>

#include "core/auth_api.hpp"
#include "core/client.hpp"
#include "core/settings.hpp"
#include "gui/main_window.hpp"
#include "gui/onboarding.hpp"

using namespace agentlink;

/**
 * 入口。
 *
 * 启动流程（像 QQ / Element 那样）：
 *   1. 读本地设置
 *   2. 有已保存的服务器 + token → 直接连，进聊天
 *   3. 没有 → 走引导：填地址 → 注册/登录 → 进聊天
 *
 * 这是"客户端在任意电脑上，服务端部署在服务器上"的关键：
 * 客户端里**没有**写死任何地址，全靠用户填 + 本地记住。
 */
int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("AgentLink"));
  QApplication::setApplicationVersion(QStringLiteral("1.0.0"));
  QApplication::setOrganizationName(QStringLiteral("AgentLink"));

  // `--version`：不进入事件循环，打印版本就退出。
  //
  // 存在的意义是**可验证性**：验收套件要证明"GUI 二进制真的能起来"
  // （Qt 平台插件、TLS 后端、动态库都在位），但它不能弹窗口等用户点。
  // 有这条，`qt-client` 套件就能在不带显示环境的情况下断言 GUI 可执行。
  // `--screenshot <path>`：渲染首屏并抓成 PNG，然后退出。
  //
  // 为什么需要它：`--version` 只能证明"二进制能起来"，证明不了**界面画得出来** ——
  // 缺字体、样式表加载失败、控件布局塌成 0×0 这类问题它一个都抓不到。
  // 有了截图，验收能断言"这张图不是空白"，等于真的渲染过一遍。
  //
  // 配 QT_QPA_PLATFORM=offscreen 用，所以不需要 Xvfb。
  QString screenshotPath;
  bool screenshotMainWindow = false;
  for (int i = 1; i < argc; ++i) {
    const QString arg = QString::fromLocal8Bit(argv[i]);
    if (arg == QStringLiteral("--screenshot") && i + 1 < argc) {
      screenshotPath = QString::fromLocal8Bit(argv[i + 1]);
    }
    // 强制抓主窗口（聊天界面）：CI 里没有已保存的登录态，否则只会抓到引导页，
    // 而聊天界面才是主界面 —— 它的布局坏了同样得有人发现。
    if (arg == QStringLiteral("--screenshot-main")) {
      screenshotMainWindow = true;
    }
  }

  for (int i = 1; i < argc; ++i) {
    const QString arg = QString::fromLocal8Bit(argv[i]);
    if (arg == QStringLiteral("--version") || arg == QStringLiteral("-v")) {
      // 带上 Qt 版本：排障时第一件要问的就是"你那个 Qt 是哪个版本"
      std::printf("agentlink-qt %s (Qt %s)\n",
                  QApplication::applicationVersion().toUtf8().constData(),
                  QT_VERSION_STR);
      return 0;
    }
  }

  SettingsStore settings;
  settings.load();

  AuthApi api;
  Client client;

  // 截图模式：渲染当前启动形态（有已存服务器 → 主窗口；否则 → 引导页）后退出。
  if (!screenshotPath.isEmpty()) {
    // 让布局与样式表先应用完，再抓图；否则可能抓到一张还没排版的空白。
    QWidget* target = nullptr;
    Onboarding onboarding(&api, &settings);
    const ServerEntry* saved = settings.activeEntry();
    if (screenshotMainWindow || (saved != nullptr && !saved->accessToken.isEmpty())) {
      auto* window = new MainWindow(&client, &api, &settings);
      window->show();
      target = window;
    } else {
      onboarding.show();
      target = &onboarding;
    }

    // 转几圈事件循环：show() 之后尺寸/字体/样式才真正落到控件上
    for (int i = 0; i < 5; ++i) {
      QApplication::processEvents();
    }
    const QPixmap shot = target->grab();
    QDir().mkpath(QFileInfo(screenshotPath).absolutePath());
    const bool saved_ok = shot.save(screenshotPath, "PNG");
    std::printf("screenshot %s %dx%d -> %s\n", saved_ok ? "ok" : "failed",
                shot.width(), shot.height(), screenshotPath.toUtf8().constData());
    return saved_ok ? 0 : 1;
  }

  // 有记住的服务器就直接连。
  const ServerEntry* active = settings.activeEntry();
  if (active != nullptr && !active->accessToken.isEmpty()) {
    const auto address = ServerAddress::parse(active->address);
    if (address.has_value()) {
      api.setHttpBase(address->httpBase());
      MainWindow window(&client, &api, &settings);
      window.show();
      client.connectTo(*address, active->accessToken);
      return app.exec();
    }
  }

  // 没有就引导。
  Onboarding onboarding(&api, &settings);
  if (onboarding.exec() != QDialog::Accepted) {
    return 0;  // 用户取消：正常退出，不是错误
  }

  const auto address = ServerAddress::parse(onboarding.serverAddress());
  if (!address.has_value()) {
    QMessageBox::warning(nullptr, QStringLiteral("地址不对"),
                         QStringLiteral("保存的服务器地址无法解析：%1").arg(onboarding.serverAddress()));
    return 1;
  }

  api.setHttpBase(address->httpBase());
  MainWindow window(&client, &api, &settings);
  window.show();
  client.connectTo(*address, onboarding.accessToken());
  return app.exec();
}
