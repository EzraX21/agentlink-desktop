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
  void repliesWithReplyTo();
  void revokesOwnMessage();
  void updatesGroupInfo();
  void searchesMessages();
  void reactsToMessage();
  void editsOwnMessage();
  void pinsMessage();
  void forwardsMessage();
  void managesMembers();

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

  /// 注册一个账号并连上（每个用例自给自足，不依赖别的用例的 token）。
  bool registerAndConnect(AuthResult& auth, Client& client, const QString& prefix) {
    AuthApi api;
    api.setHttpBase(baseUrl_);
    const QString username = QStringLiteral("%1_%2")
                                 .arg(prefix)
                                 .arg(QDateTime::currentMSecsSinceEpoch() % 100000);
    bool done = false;
    api.registerUser(username, prefix, QStringLiteral("qt-client-pass-1234"),
                     [&](const AuthResult& r) {
                       auth = r;
                       done = true;
                     });
    if (!waitFor([&] { return done; }) || !auth.ok) return false;

    bool ready = false;
    client.setStateHandler([&](ConnectionState state, const QString&) {
      if (state == ConnectionState::Ready) ready = true;
    });
    const auto address = ServerAddress::parse(baseUrl_);
    if (!address.has_value()) return false;
    client.connectTo(*address, auth.accessToken);
    return waitFor([&] { return ready; }, 15000);
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

void TestE2E::repliesWithReplyTo() {
  AuthResult auth;
  Client client;
  QVERIFY(registerAndConnect(auth, client, QStringLiteral("qt_reply")));

  // 建群
  QVariantMap conv;
  bool convDone = false;
  client.send(QStringLiteral("conv.create"),
              QVariantMap{{QStringLiteral("kind"), QStringLiteral("group")},
                          {QStringLiteral("title"), QStringLiteral("回复测试")}},
              [&](bool ok, const QVariantMap& data, const WireError&) {
                QVERIFY(ok);
                conv = data.value(QStringLiteral("conv")).toMap();
                convDone = true;
              });
  QVERIFY(waitFor([&] { return convDone; }));
  const QString convId = conv.value(QStringLiteral("id")).toString();

  // 发第一条
  QString firstId;
  bool firstDone = false;
  client.send(QStringLiteral("message.send"),
              QVariantMap{{QStringLiteral("conv_id"), convId},
                          {QStringLiteral("client_msg_id"), QStringLiteral("qt-reply-a")},
                          {QStringLiteral("content"),
                           QVariantList{QVariantMap{{QStringLiteral("type"), QStringLiteral("text")},
                                                   {QStringLiteral("text"), QStringLiteral("被回复的那条")}}}}},
              [&](bool ok, const QVariantMap& data, const WireError&) {
                QVERIFY(ok);
                firstId = data.value(QStringLiteral("message_id")).toString();
                firstDone = true;
              });
  QVERIFY(waitFor([&] { return firstDone; }));

  // 第二条带 reply_to
  bool replyDone = false;
  client.send(QStringLiteral("message.send"),
              QVariantMap{{QStringLiteral("conv_id"), convId},
                          {QStringLiteral("client_msg_id"), QStringLiteral("qt-reply-b")},
                          {QStringLiteral("reply_to"), firstId},
                          {QStringLiteral("content"),
                           QVariantList{QVariantMap{{QStringLiteral("type"), QStringLiteral("text")},
                                                   {QStringLiteral("text"), QStringLiteral("这是一条回复")}}}}},
              [&](bool ok, const QVariantMap&, const WireError&) {
                QVERIFY(ok);
                replyDone = true;
              });
  QVERIFY(waitFor([&] { return replyDone; }));

  // 历史里第二条的 reply_to 指向第一条
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

  bool found = false;
  const QVariantList messages = history.value(QStringLiteral("messages")).toList();
  for (const QVariant& m : messages) {
    const QVariantMap msg = m.toMap();
    if (msg.value(QStringLiteral("id")).toString() == firstId) continue;
    if (msg.value(QStringLiteral("reply_to")).toString() == firstId) found = true;
  }
  QVERIFY(found);
}

void TestE2E::revokesOwnMessage() {
  AuthResult auth;
  Client client;
  QVERIFY(registerAndConnect(auth, client, QStringLiteral("qt_revoke")));

  QVariantMap conv;
  bool convDone = false;
  client.send(QStringLiteral("conv.create"),
              QVariantMap{{QStringLiteral("kind"), QStringLiteral("group")},
                          {QStringLiteral("title"), QStringLiteral("撤回测试")}},
              [&](bool ok, const QVariantMap& data, const WireError&) {
                QVERIFY(ok);
                conv = data.value(QStringLiteral("conv")).toMap();
                convDone = true;
              });
  QVERIFY(waitFor([&] { return convDone; }));
  const QString convId = conv.value(QStringLiteral("id")).toString();

  QString sentId;
  bool sendDone = false;
  client.send(QStringLiteral("message.send"),
              QVariantMap{{QStringLiteral("conv_id"), convId},
                          {QStringLiteral("client_msg_id"), QStringLiteral("qt-revoke-1")},
                          {QStringLiteral("content"),
                           QVariantList{QVariantMap{{QStringLiteral("type"), QStringLiteral("text")},
                                                   {QStringLiteral("text"), QStringLiteral("这条会被撤回")}}}}},
              [&](bool ok, const QVariantMap& data, const WireError&) {
                QVERIFY(ok);
                sentId = data.value(QStringLiteral("message_id")).toString();
                sendDone = true;
              });
  QVERIFY(waitFor([&] { return sendDone; }));

  bool revokeDone = false;
  client.send(QStringLiteral("message.revoke"),
              QVariantMap{{QStringLiteral("conv_id"), convId}, {QStringLiteral("message_id"), sentId}},
              [&](bool ok, const QVariantMap&, const WireError&) {
                QVERIFY(ok);
                revokeDone = true;
              });
  QVERIFY(waitFor([&] { return revokeDone; }));

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

  // 服务端 history.fetch 会过滤掉已撤回的消息（state='revoked' 不进列表）。
  // 所以正确断言是：这条消息**不在**历史里了。
  const QVariantList messages = history.value(QStringLiteral("messages")).toList();
  for (const QVariant& m : messages) {
    const QVariantMap msg = m.toMap();
    QVERIFY2(msg.value(QStringLiteral("id")).toString() != sentId,
             "撤回的消息不该出现在历史里");
  }
}

void TestE2E::updatesGroupInfo() {
  AuthResult auth;
  Client client;
  QVERIFY(registerAndConnect(auth, client, QStringLiteral("qt_groupinfo")));

  QVariantMap conv;
  bool convDone = false;
  client.send(QStringLiteral("conv.create"),
              QVariantMap{{QStringLiteral("kind"), QStringLiteral("group")},
                          {QStringLiteral("title"), QStringLiteral("旧名")}},
              [&](bool ok, const QVariantMap& data, const WireError&) {
                QVERIFY(ok);
                conv = data.value(QStringLiteral("conv")).toMap();
                convDone = true;
              });
  QVERIFY(waitFor([&] { return convDone; }));
  const QString convId = conv.value(QStringLiteral("id")).toString();

  QVariantMap updated;
  bool updDone = false;
  client.send(QStringLiteral("group.update"),
              QVariantMap{{QStringLiteral("conv_id"), convId},
                          {QStringLiteral("title"), QStringLiteral("新名")},
                          {QStringLiteral("announcement"), QStringLiteral("新公告")}},
              [&](bool ok, const QVariantMap& data, const WireError&) {
                QVERIFY(ok);
                updated = data.value(QStringLiteral("conv")).toMap();
                updDone = true;
              });
  QVERIFY(waitFor([&] { return updDone; }));
  QCOMPARE(updated.value(QStringLiteral("title")).toString(), QStringLiteral("新名"));
  QCOMPARE(updated.value(QStringLiteral("announcement")).toString(), QStringLiteral("新公告"));
}

void TestE2E::searchesMessages() {
  AuthResult auth;
  Client client;
  QVERIFY(registerAndConnect(auth, client, QStringLiteral("qt_search")));

  QVariantMap conv;
  bool convDone = false;
  client.send(QStringLiteral("conv.create"),
              QVariantMap{{QStringLiteral("kind"), QStringLiteral("group")},
                          {QStringLiteral("title"), QStringLiteral("搜索测试")}},
              [&](bool ok, const QVariantMap& data, const WireError&) {
                QVERIFY(ok);
                conv = data.value(QStringLiteral("conv")).toMap();
                convDone = true;
              });
  QVERIFY(waitFor([&] { return convDone; }));
  const QString convId = conv.value(QStringLiteral("id")).toString();

  const QString keyword = QStringLiteral("唯一关键词%1").arg(QDateTime::currentMSecsSinceEpoch());
  bool sendDone = false;
  client.send(QStringLiteral("message.send"),
              QVariantMap{{QStringLiteral("conv_id"), convId},
                          {QStringLiteral("client_msg_id"), QStringLiteral("qt-search-1")},
                          {QStringLiteral("content"),
                           QVariantList{QVariantMap{{QStringLiteral("type"), QStringLiteral("text")},
                                                   {QStringLiteral("text"), keyword}}}}},
              [&](bool ok, const QVariantMap&, const WireError&) {
                QVERIFY(ok);
                sendDone = true;
              });
  QVERIFY(waitFor([&] { return sendDone; }));

  QVariantMap result;
  bool searchDone = false;
  client.send(QStringLiteral("search.messages"),
              QVariantMap{{QStringLiteral("q"), keyword}, {QStringLiteral("limit"), 10}},
              [&](bool ok, const QVariantMap& data, const WireError&) {
                QVERIFY(ok);
                result = data;
                searchDone = true;
              });
  QVERIFY(waitFor([&] { return searchDone; }));

  const QVariantList hits = result.value(QStringLiteral("hits")).toList();
  bool found = false;
  for (const QVariant& h : hits) {
    const QVariantMap hit = h.toMap();
    if (hit.value(QStringLiteral("conv_id")).toString() == convId) found = true;
  }
  QVERIFY(found);
}

void TestE2E::reactsToMessage() {
  AuthResult auth;
  Client client;
  QVERIFY(registerAndConnect(auth, client, QStringLiteral("qt_react")));

  QVariantMap conv;
  bool convDone = false;
  client.send(QStringLiteral("conv.create"),
              QVariantMap{{QStringLiteral("kind"), QStringLiteral("group")},
                          {QStringLiteral("title"), QStringLiteral("回应测试")}},
              [&](bool ok, const QVariantMap& data, const WireError&) {
                QVERIFY(ok);
                conv = data.value(QStringLiteral("conv")).toMap();
                convDone = true;
              });
  QVERIFY(waitFor([&] { return convDone; }));
  const QString convId = conv.value(QStringLiteral("id")).toString();

  QString msgId;
  bool sendDone = false;
  client.send(QStringLiteral("message.send"),
              QVariantMap{{QStringLiteral("conv_id"), convId},
                          {QStringLiteral("client_msg_id"), QStringLiteral("qt-react-1")},
                          {QStringLiteral("content"),
                           QVariantList{QVariantMap{{QStringLiteral("type"), QStringLiteral("text")},
                                                   {QStringLiteral("text"), QStringLiteral("来点个赞")}}}}},
              [&](bool ok, const QVariantMap& data, const WireError&) {
                QVERIFY(ok);
                msgId = data.value(QStringLiteral("message_id")).toString();
                sendDone = true;
              });
  QVERIFY(waitFor([&] { return sendDone; }));

  bool reactDone = false;
  client.send(QStringLiteral("reaction.add"),
              QVariantMap{{QStringLiteral("conv_id"), convId},
                          {QStringLiteral("message_id"), msgId},
                          {QStringLiteral("emoji"), QStringLiteral("👍")}},
              [&](bool ok, const QVariantMap&, const WireError&) {
                QVERIFY(ok);
                reactDone = true;
              });
  QVERIFY(waitFor([&] { return reactDone; }));

  // history 里这条消息的 reactions 有 👍 且包含自己
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
  for (const QVariant& m : messages) {
    const QVariantMap msg = m.toMap();
    if (msg.value(QStringLiteral("id")).toString() != msgId) continue;
    const QVariantList reactions = msg.value(QStringLiteral("reactions")).toList();
    for (const QVariant& r : reactions) {
      const QVariantMap reaction = r.toMap();
      if (reaction.value(QStringLiteral("emoji")).toString() == QStringLiteral("👍")) {
        QVERIFY(reaction.value(QStringLiteral("user_ids")).toList().contains(auth.userId));
        return;
      }
    }
    QVERIFY2(false, "没找到 👍 回应");
  }
  QVERIFY2(false, "没找到消息");
}

void TestE2E::editsOwnMessage() {
  AuthResult auth;
  Client client;
  QVERIFY(registerAndConnect(auth, client, QStringLiteral("qt_edit")));

  QVariantMap conv;
  bool convDone = false;
  client.send(QStringLiteral("conv.create"),
              QVariantMap{{QStringLiteral("kind"), QStringLiteral("group")},
                          {QStringLiteral("title"), QStringLiteral("编辑测试")}},
              [&](bool ok, const QVariantMap& data, const WireError&) {
                QVERIFY(ok);
                conv = data.value(QStringLiteral("conv")).toMap();
                convDone = true;
              });
  QVERIFY(waitFor([&] { return convDone; }));
  const QString convId = conv.value(QStringLiteral("id")).toString();

  QString msgId;
  bool sendDone = false;
  client.send(QStringLiteral("message.send"),
              QVariantMap{{QStringLiteral("conv_id"), convId},
                          {QStringLiteral("client_msg_id"), QStringLiteral("qt-edit-1")},
                          {QStringLiteral("content"),
                           QVariantList{QVariantMap{{QStringLiteral("type"), QStringLiteral("text")},
                                                   {QStringLiteral("text"), QStringLiteral("原文")}}}}},
              [&](bool ok, const QVariantMap& data, const WireError&) {
                QVERIFY(ok);
                msgId = data.value(QStringLiteral("message_id")).toString();
                sendDone = true;
              });
  QVERIFY(waitFor([&] { return sendDone; }));

  bool editDone = false;
  client.send(QStringLiteral("message.edit"),
              QVariantMap{{QStringLiteral("conv_id"), convId},
                          {QStringLiteral("message_id"), msgId},
                          {QStringLiteral("content"),
                           QVariantList{QVariantMap{{QStringLiteral("type"), QStringLiteral("text")},
                                                   {QStringLiteral("text"), QStringLiteral("改过的")}}}}},
              [&](bool ok, const QVariantMap&, const WireError&) {
                QVERIFY(ok);
                editDone = true;
              });
  QVERIFY(waitFor([&] { return editDone; }));

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
  for (const QVariant& m : messages) {
    const QVariantMap msg = m.toMap();
    if (msg.value(QStringLiteral("id")).toString() != msgId) continue;
    const QVariantList content = msg.value(QStringLiteral("content")).toList();
    QCOMPARE(content.at(0).toMap().value(QStringLiteral("text")).toString(), QStringLiteral("改过的"));
    QVERIFY(!msg.value(QStringLiteral("edited_at")).isNull());
    return;
  }
  QVERIFY2(false, "没找到消息");
}

void TestE2E::pinsMessage() {
  AuthResult auth;
  Client client;
  QVERIFY(registerAndConnect(auth, client, QStringLiteral("qt_pin")));

  QVariantMap conv;
  bool convDone = false;
  client.send(QStringLiteral("conv.create"),
              QVariantMap{{QStringLiteral("kind"), QStringLiteral("group")},
                          {QStringLiteral("title"), QStringLiteral("置顶测试")}},
              [&](bool ok, const QVariantMap& data, const WireError&) {
                QVERIFY(ok);
                conv = data.value(QStringLiteral("conv")).toMap();
                convDone = true;
              });
  QVERIFY(waitFor([&] { return convDone; }));
  const QString convId = conv.value(QStringLiteral("id")).toString();

  QString msgId;
  bool sendDone = false;
  client.send(QStringLiteral("message.send"),
              QVariantMap{{QStringLiteral("conv_id"), convId},
                          {QStringLiteral("client_msg_id"), QStringLiteral("qt-pin-1")},
                          {QStringLiteral("content"),
                           QVariantList{QVariantMap{{QStringLiteral("type"), QStringLiteral("text")},
                                                   {QStringLiteral("text"), QStringLiteral("这条置顶")}}}}},
              [&](bool ok, const QVariantMap& data, const WireError&) {
                QVERIFY(ok);
                msgId = data.value(QStringLiteral("message_id")).toString();
                sendDone = true;
              });
  QVERIFY(waitFor([&] { return sendDone; }));

  // 建群者就是 owner，有置顶权限
  bool pinDone = false;
  client.send(QStringLiteral("message.pin"),
              QVariantMap{{QStringLiteral("conv_id"), convId}, {QStringLiteral("message_id"), msgId}},
              [&](bool ok, const QVariantMap&, const WireError&) {
                QVERIFY(ok);
                pinDone = true;
              });
  QVERIFY(waitFor([&] { return pinDone; }));

  QVariantMap convGet;
  bool getDone = false;
  client.send(QStringLiteral("conv.get"), QVariantMap{{QStringLiteral("conv_id"), convId}},
              [&](bool ok, const QVariantMap& data, const WireError&) {
                QVERIFY(ok);
                convGet = data;
                getDone = true;
              });
  QVERIFY(waitFor([&] { return getDone; }));
  QCOMPARE(convGet.value(QStringLiteral("conv")).toMap()
               .value(QStringLiteral("pinned_message_id")).toString(),
           msgId);
}

void TestE2E::forwardsMessage() {
  AuthResult auth;
  Client client;
  QVERIFY(registerAndConnect(auth, client, QStringLiteral("qt_fwd")));

  // 两个群
  QVariantMap convA;
  bool aDone = false;
  client.send(QStringLiteral("conv.create"),
              QVariantMap{{QStringLiteral("kind"), QStringLiteral("group")},
                          {QStringLiteral("title"), QStringLiteral("群A")}},
              [&](bool ok, const QVariantMap& data, const WireError&) {
                QVERIFY(ok);
                convA = data.value(QStringLiteral("conv")).toMap();
                aDone = true;
              });
  QVERIFY(waitFor([&] { return aDone; }));

  QVariantMap convB;
  bool bDone = false;
  client.send(QStringLiteral("conv.create"),
              QVariantMap{{QStringLiteral("kind"), QStringLiteral("group")},
                          {QStringLiteral("title"), QStringLiteral("群B")}},
              [&](bool ok, const QVariantMap& data, const WireError&) {
                QVERIFY(ok);
                convB = data.value(QStringLiteral("conv")).toMap();
                bDone = true;
              });
  QVERIFY(waitFor([&] { return bDone; }));

  QString msgId;
  bool sendDone = false;
  client.send(QStringLiteral("message.send"),
              QVariantMap{{QStringLiteral("conv_id"), convA.value(QStringLiteral("id")).toString()},
                          {QStringLiteral("client_msg_id"), QStringLiteral("qt-fwd-1")},
                          {QStringLiteral("content"),
                           QVariantList{QVariantMap{{QStringLiteral("type"), QStringLiteral("text")},
                                                   {QStringLiteral("text"), QStringLiteral("转发我")}}}}},
              [&](bool ok, const QVariantMap& data, const WireError&) {
                QVERIFY(ok);
                msgId = data.value(QStringLiteral("message_id")).toString();
                sendDone = true;
              });
  QVERIFY(waitFor([&] { return sendDone; }));

  bool fwdDone = false;
  client.send(QStringLiteral("message.forward"),
              QVariantMap{{QStringLiteral("conv_id"), convB.value(QStringLiteral("id")).toString()},
                          {QStringLiteral("from_conv_id"), convA.value(QStringLiteral("id")).toString()},
                          {QStringLiteral("message_id"), msgId}},
              [&](bool ok, const QVariantMap&, const WireError&) {
                QVERIFY(ok);
                fwdDone = true;
              });
  QVERIFY(waitFor([&] { return fwdDone; }));

  QVariantMap history;
  bool histDone = false;
  client.send(QStringLiteral("history.fetch"),
              QVariantMap{{QStringLiteral("conv_id"), convB.value(QStringLiteral("id")).toString()},
                          {QStringLiteral("limit"), 10}},
              [&](bool ok, const QVariantMap& data, const WireError&) {
                QVERIFY(ok);
                history = data;
                histDone = true;
              });
  QVERIFY(waitFor([&] { return histDone; }));

  const QVariantList messages = history.value(QStringLiteral("messages")).toList();
  for (const QVariant& m : messages) {
    const QVariantMap msg = m.toMap();
    const QVariantList content = msg.value(QStringLiteral("content")).toList();
    if (content.size() > 0 &&
        content.at(0).toMap().value(QStringLiteral("text")).toString() == QStringLiteral("转发我")) {
      return;  // 找到了，转发成功
    }
  }
  QVERIFY2(false, "转发的内容没出现在目标群历史里");
}

void TestE2E::managesMembers() {
  AuthResult auth;
  Client client;
  QVERIFY(registerAndConnect(auth, client, QStringLiteral("qt_member")));

  // 另一个人，用来被邀请/踢出
  AuthApi api;
  api.setHttpBase(baseUrl_);
  AuthResult other;
  bool otherDone = false;
  api.registerUser(
      QStringLiteral("qt_member_b_%1").arg(QDateTime::currentMSecsSinceEpoch() % 100000),
      QStringLiteral("被管理者"), QStringLiteral("qt-client-pass-1234"),
      [&](const AuthResult& r) {
        other = r;
        otherDone = true;
      });
  QVERIFY(waitFor([&] { return otherDone; }));
  QVERIFY2(other.ok, qPrintable(other.error));

  QVariantMap conv;
  bool convDone = false;
  client.send(QStringLiteral("conv.create"),
              QVariantMap{{QStringLiteral("kind"), QStringLiteral("group")},
                          {QStringLiteral("title"), QStringLiteral("成员管理测试")}},
              [&](bool ok, const QVariantMap& data, const WireError&) {
                QVERIFY(ok);
                conv = data.value(QStringLiteral("conv")).toMap();
                convDone = true;
              });
  QVERIFY(waitFor([&] { return convDone; }));
  const QString convId = conv.value(QStringLiteral("id")).toString();

  // 邀请
  QVariantMap invited;
  bool inviteDone = false;
  client.send(QStringLiteral("member.invite"),
              QVariantMap{{QStringLiteral("conv_id"), convId},
                          {QStringLiteral("user_ids"), QStringList{other.userId}}},
              [&](bool ok, const QVariantMap& data, const WireError&) {
                QVERIFY(ok);
                invited = data;
                inviteDone = true;
              });
  QVERIFY(waitFor([&] { return inviteDone; }));
  bool found = false;
  for (const QVariant& m : invited.value(QStringLiteral("members")).toList()) {
    if (m.toMap().value(QStringLiteral("user_id")).toString() == other.userId) found = true;
  }
  QVERIFY2(found, "邀请后成员列表里有新成员");

  // 设为管理员 → 角色变了
  bool promoteDone = false;
  client.send(QStringLiteral("member.role.set"),
              QVariantMap{{QStringLiteral("conv_id"), convId},
                          {QStringLiteral("user_id"), other.userId},
                          {QStringLiteral("role"), QStringLiteral("admin")}},
              [&](bool ok, const QVariantMap&, const WireError&) {
                QVERIFY(ok);
                promoteDone = true;
              });
  QVERIFY(waitFor([&] { return promoteDone; }));

  {
    QVariantMap got;
    bool gotDone = false;
    client.send(QStringLiteral("conv.get"), QVariantMap{{QStringLiteral("conv_id"), convId}},
                [&](bool ok, const QVariantMap& data, const WireError&) {
                  QVERIFY(ok);
                  got = data;
                  gotDone = true;
                });
    QVERIFY(waitFor([&] { return gotDone; }));
    bool isAdmin = false;
    for (const QVariant& m : got.value(QStringLiteral("members")).toList()) {
      const QVariantMap member = m.toMap();
      if (member.value(QStringLiteral("user_id")).toString() == other.userId &&
          member.value(QStringLiteral("role")).toString() == QStringLiteral("admin")) {
        isAdmin = true;
      }
    }
    QVERIFY2(isAdmin, "设为管理员后角色是 admin");
  }

  // 禁言 → muted_until > now
  const double until = static_cast<double>(QDateTime::currentMSecsSinceEpoch() + 600'000);
  bool muteDone = false;
  client.send(QStringLiteral("member.mute"),
              QVariantMap{{QStringLiteral("conv_id"), convId},
                          {QStringLiteral("user_id"), other.userId},
                          {QStringLiteral("until_ms"), until}},
              [&](bool ok, const QVariantMap&, const WireError&) {
                QVERIFY(ok);
                muteDone = true;
              });
  QVERIFY(waitFor([&] { return muteDone; }));

  // 踢出 → 不在成员列表
  bool kickDone = false;
  client.send(QStringLiteral("member.kick"),
              QVariantMap{{QStringLiteral("conv_id"), convId},
                          {QStringLiteral("user_id"), other.userId}},
              [&](bool ok, const QVariantMap&, const WireError&) {
                QVERIFY(ok);
                kickDone = true;
              });
  QVERIFY(waitFor([&] { return kickDone; }));

  QVariantMap after;
  bool afterDone = false;
  client.send(QStringLiteral("conv.get"), QVariantMap{{QStringLiteral("conv_id"), convId}},
              [&](bool ok, const QVariantMap& data, const WireError&) {
                QVERIFY(ok);
                after = data;
                afterDone = true;
              });
  QVERIFY(waitFor([&] { return afterDone; }));
  bool stillThere = false;
  for (const QVariant& m : after.value(QStringLiteral("members")).toList()) {
    if (m.toMap().value(QStringLiteral("user_id")).toString() == other.userId) stillThere = true;
  }
  QVERIFY2(!stillThere, "踢出后不在成员列表");
}

QTEST_GUILESS_MAIN(TestE2E)
#include "test_e2e.moc"
