#include <QtTest>

#include "core/client.hpp"

using namespace agentlink;

/**
 * 服务器地址解析测试。
 *
 * 这一层决定"用户填的地址能不能用"—— 用户最常犯的错就是漏了协议头
 * 或者把 https 填进来（浏览器地址栏那个）。
 */
class TestAddress : public QObject {
  Q_OBJECT

 private slots:
  void parsesBareHost();
  void parsesWs();
  void parsesWss();
  void upgradesHttpToWs();
  void upgradesHttpsToWss();
  void keepsPort();
  void addsDefaultWsPath();
  void rejectsGarbage();
  void derivesHttpBase();
  void displayIsReadable();
};

void TestAddress::parsesBareHost() {
  const auto a = ServerAddress::parse(QStringLiteral("chat.example.com"));
  QVERIFY(a.has_value());
  QCOMPARE(a->url, QStringLiteral("ws://chat.example.com/ws"));
}

void TestAddress::parsesWs() {
  const auto a = ServerAddress::parse(QStringLiteral("ws://127.0.0.1:3000"));
  QVERIFY(a.has_value());
  QCOMPARE(a->url, QStringLiteral("ws://127.0.0.1:3000/ws"));
}

void TestAddress::parsesWss() {
  const auto a = ServerAddress::parse(QStringLiteral("wss://chat.example.com"));
  QVERIFY(a.has_value());
  QCOMPARE(a->url, QStringLiteral("wss://chat.example.com/ws"));
}

void TestAddress::upgradesHttpToWs() {
  const auto a = ServerAddress::parse(QStringLiteral("http://127.0.0.1:3000"));
  QVERIFY(a.has_value());
  QCOMPARE(a->url, QStringLiteral("ws://127.0.0.1:3000/ws"));
}

void TestAddress::upgradesHttpsToWss() {
  const auto a = ServerAddress::parse(QStringLiteral("https://chat.example.com"));
  QVERIFY(a.has_value());
  QCOMPARE(a->url, QStringLiteral("wss://chat.example.com/ws"));
}

void TestAddress::keepsPort() {
  const auto a = ServerAddress::parse(QStringLiteral("https://chat.example.com:8443"));
  QVERIFY(a.has_value());
  QCOMPARE(a->url, QStringLiteral("wss://chat.example.com:8443/ws"));
}

void TestAddress::addsDefaultWsPath() {
  // 只写 /ws 的地址不该变成 /ws/ws
  const auto a = ServerAddress::parse(QStringLiteral("ws://chat.example.com/ws"));
  QVERIFY(a.has_value());
  QCOMPARE(a->url, QStringLiteral("ws://chat.example.com/ws"));
}

void TestAddress::rejectsGarbage() {
  QVERIFY(!ServerAddress::parse(QString()).has_value());
  QVERIFY(!ServerAddress::parse(QStringLiteral("   ")).has_value());
  QVERIFY(!ServerAddress::parse(QStringLiteral("ftp://example.com")).has_value());
}

void TestAddress::derivesHttpBase() {
  const auto a = ServerAddress::parse(QStringLiteral("https://chat.example.com"));
  QVERIFY(a.has_value());
  QCOMPARE(a->httpBase(), QStringLiteral("https://chat.example.com"));

  const auto b = ServerAddress::parse(QStringLiteral("ws://127.0.0.1:3000"));
  QVERIFY(b.has_value());
  QCOMPARE(b->httpBase(), QStringLiteral("http://127.0.0.1:3000"));
}

void TestAddress::displayIsReadable() {
  const auto a = ServerAddress::parse(QStringLiteral("wss://chat.example.com/ws"));
  QVERIFY(a.has_value());
  QCOMPARE(a->display(), QStringLiteral("chat.example.com/ws"));
}

QTEST_GUILESS_MAIN(TestAddress)
#include "test_address.moc"
