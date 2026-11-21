#include <QtTest>

#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QUrl>

#include "core/auth_api.hpp"
#include "core/client.hpp"

using namespace agentlink;

/**
 * 端到端：Qt 客户端**真连**一个 AgentLink 服务端。
 *
 * 这条测试的价值在于"客户端和 Web 客户端说的是不是同一套协议"：
 * 如果 Qt 侧的帧格式错了、握手顺序错了，这里会红。
 *
 * 需要环境变量 `AGENTLINK_TEST_URL`（形如 `http://127.0.0.1:3000`）与
 * `AGENTLINK_TEST_TOKEN`。没有就**跳过**（退出码 77），不是失败 ——
 * 因为它是"按需"套件，不该挡住没有服务端的环境。
 */
class TestE2E : public QObject {
  Q_OBJECT

 private slots:
  void initTestCase();
  void registersAndAuthenticates();
  void sendsAndReceivesMessage();
  void listsConversations();
  void uploadsAttachment();

 private:
  /// 跑一个事件循环直到 predicate 为真或超时。
  template <typename Predicate>
  bool waitFor(Predicate predicate, int timeoutMs = 10000) {
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&loop, &predicate]() {
      if (predicate()) loop.quit();
    });
    poll.start(50);
    timer.start(timeoutMs);
    loop.exec();
    return predicate();
  }

  QString baseUrl_;
  QString wsUrl_;
};

void TestE2E::initTestCase() {
  baseUrl_ = qEnvironmentVariable("AGENTLINK_TEST_URL");
  wsUrl_ = qEnvironmentVariable("AGENTLINK_TEST_WS_URL");
  if (baseUrl_.isEmpty() || wsUrl_.isEmpty()) {
    QSKIP("没有 AGENTLINK_TEST_URL / AGENTLINK_TEST_WS_URL，跳过端到端测试");
  }
}

void TestE2E::registersAndAuthenticates() {
  AuthApi api;
  api.setHttpBase(baseUrl_);

  const QString username = QStringLiteral("qt_client_%1")
                               .arg(QDateTime::currentMSecsSinceEpoch() % 100000);
  AuthResult result;
  bool done = false;
  api.registerUser(username, QStringLiteral("Qt 客户端"), QStringLiteral("qt-client-pass-1234"),
                   [&](const AuthResult& r) {
                     result = r;
                     done = true;
                   });
  QVERIFY(waitFor([&] { return done; }));
  QVERIFY2(result.ok, qPrintable(result.error));
  QVERIFY(!result.accessToken.isEmpty());
  QVERIFY(!result.userId.isEmpty());

  // 用拿到的 token 连 WS 并认证
  Client client;
  QVariantMap self;
  bool ready = false;
  client.setStateHandler([&](ConnectionState state, const QString&) {
    if (state == ConnectionState::Ready) ready = true;
  });

  const auto address = ServerAddress::parse(baseUrl_);
  QVERIFY(address.has_value());
  client.connectTo(*address, result.accessToken);
  QVERIFY(waitFor([&] { return ready; }, 15000));

  QVERIFY(!client.selfId().isEmpty());
  QCOMPARE(client.state(), ConnectionState::Ready);
}

void TestE2E::sendsAndReceivesMessage() {
  AuthApi api;
  api.setHttpBase(baseUrl_);

  // 注册一个账号 + 建群 + 拿到 token
  const QString username = QStringLiteral("qt_e2e_%1")
                               .arg(QDateTime::currentMSecsSinceEpoch() % 100000);
  AuthResult reg;
  bool done = false;
  api.registerUser(username, QStringLiteral("E2E"), QStringLiteral("qt-client-pass-1234"),
                   [&](const AuthResult& r) {
                     reg = r;
                     done = true;
                   });
  QVERIFY(waitFor([&] { return done; }));
  QVERIFY2(reg.ok, qPrintable(reg.error));

  Client client;
  bool ready = false;
  client.setStateHandler([&](ConnectionState state, const QString&) {
    if (state == ConnectionState::Ready) ready = true;
  });
  const auto address = ServerAddress::parse(baseUrl_);
  QVERIFY(address.has_value());
  client.connectTo(*address, reg.accessToken);
  QVERIFY(waitFor([&] { return ready; }, 15000));

  // 建群
  QVariantMap conv;
  bool convDone = false;
  client.send(QStringLiteral("conv.create"),
              QVariantMap{{QStringLiteral("kind"), QStringLiteral("group")},
                          {QStringLiteral("title"), QStringLiteral("Qt 端到端群")}},
              [&](bool ok, const QVariantMap& data, const WireError& err) {
                QVERIFY(ok);
                conv = data.value(QStringLiteral("conv")).toMap();
                convDone = true;
                Q_UNUSED(err)
              });
  QVERIFY(waitFor([&] { return convDone; }));
  const QString convId = conv.value(QStringLiteral("id")).toString();
  QVERIFY(!convId.isEmpty());

  // 发一条消息，并等自己收到服务端推回来的那条
  QString receivedId;
  client.setEventHandler([&](const Frame& frame) {
    if (frame.type != QStringLiteral("message.new")) return;
    const QVariantMap message = frame.eventData.value(QStringLiteral("message")).toMap();
    if (message.value(QStringLiteral("conv_id")).toString() == convId) {
      receivedId = message.value(QStringLiteral("id")).toString();
    }
  });

  QString sentId;
  bool sendDone = false;
  client.send(QStringLiteral("message.send"),
              QVariantMap{{QStringLiteral("conv_id"), convId},
                          {QStringLiteral("client_msg_id"), QStringLiteral("qt-e2e-1")},
                          {QStringLiteral("content"),
                           QVariantList{QVariantMap{{QStringLiteral("type"), QStringLiteral("text")},
                                                   {QStringLiteral("text"), QStringLiteral("来自 Qt 客户端")}}}}},
              [&](bool ok, const QVariantMap& data, const WireError& err) {
                QVERIFY(ok);
                sentId = data.value(QStringLiteral("message_id")).toString();
                sendDone = true;
                Q_UNUSED(err)
              });
  QVERIFY(waitFor([&] { return sendDone; }));
  QVERIFY(!sentId.isEmpty());

  QVERIFY(waitFor([&] { return !receivedId.isEmpty(); }));
  QCOMPARE(receivedId, sentId);

  // 历史里也有它
  QVariantMap history;
  bool histDone = false;
  client.send(QStringLiteral("history.fetch"),
              QVariantMap{{QStringLiteral("conv_id"), convId}, {QStringLiteral("limit"), 10}},
              [&](bool ok, const QVariantMap& data, const WireError&) {
                QVERIFY(ok);
                history = data;
                histDone = true;
              });
  QVERIFY(waitFor([&] { return histDone; }));
  const QVariantList messages = history.value(QStringLiteral("messages")).toList();
  bool found = false;
  for (const QVariant& m : messages) {
    if (m.toMap().value(QStringLiteral("id")).toString() == sentId) found = true;
  }
  QVERIFY(found);
}

void TestE2E::listsConversations() {
  // 自己注册一个账号 —— 不要依赖别的用例留下的 token：
  // 用例之间有顺序依赖的话，单跑一个就会挂，排查时很误导人。
  AuthApi api;
  api.setHttpBase(baseUrl_);
  const QString username = QStringLiteral("qt_list_%1")
                               .arg(QDateTime::currentMSecsSinceEpoch() % 100000);
  AuthResult reg;
  bool regDone = false;
  api.registerUser(username, QStringLiteral("列表"), QStringLiteral("qt-client-pass-1234"),
                   [&](const AuthResult& r) {
                     reg = r;
                     regDone = true;
                   });
  QVERIFY(waitFor([&] { return regDone; }));
  QVERIFY2(reg.ok, qPrintable(reg.error));

  Client client;
  bool ready = false;
  client.setStateHandler([&](ConnectionState state, const QString&) {
    if (state == ConnectionState::Ready) ready = true;
  });
  const auto address = ServerAddress::parse(baseUrl_);
  QVERIFY(address.has_value());
  client.connectTo(*address, reg.accessToken);
  QVERIFY(waitFor([&] { return ready; }, 15000));

  QVariantMap res;
  bool done = false;
  client.send(QStringLiteral("conv.list"), QVariantMap(),
              [&](bool ok, const QVariantMap& data, const WireError&) {
                QVERIFY(ok);
                res = data;
                done = true;
              });
  QVERIFY(waitFor([&] { return done; }));
  QVERIFY(res.contains(QStringLiteral("items")));
  // 新注册的账号还没有任何会话 —— items 应该是空数组（不是缺字段）。
  QCOMPARE(res.value(QStringLiteral("items")).toList().size(), 0);
}

void TestE2E::uploadsAttachment() {
  AuthApi api;
  api.setHttpBase(baseUrl_);

  const QString username = QStringLiteral("qt_att_%1")
                               .arg(QDateTime::currentMSecsSinceEpoch() % 100000);
  AuthResult reg;
  bool done = false;
  api.registerUser(username, QStringLiteral("附件"), QStringLiteral("qt-client-pass-1234"),
                   [&](const AuthResult& r) {
                     reg = r;
                     done = true;
                   });
  QVERIFY(waitFor([&] { return done; }));
  QVERIFY2(reg.ok, qPrintable(reg.error));

  // 造一个临时文件
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString filePath = dir.filePath(QStringLiteral("hello.txt"));
  QFile file(filePath);
  QVERIFY(file.open(QIODevice::WriteOnly));
  file.write("hello agentlink");
  file.close();

  QVariantMap info;
  bool uploadDone = false;
  api.uploadAttachment(reg.accessToken, filePath, [&](bool ok, const QVariantMap& i, const QString& err) {
    QVERIFY(ok);
    info = i;
    uploadDone = true;
    Q_UNUSED(err)
  });
  QVERIFY(waitFor([&] { return uploadDone; }));
  QVERIFY(!info.value(QStringLiteral("attachment_id")).toString().isEmpty());
  QCOMPARE(info.value(QStringLiteral("filename")).toString(), QStringLiteral("hello.txt"));
}

QTEST_GUILESS_MAIN(TestE2E)
#include "test_e2e.moc"
