#include <QtTest>

#include "core/frames.hpp"

using namespace agentlink;

/**
 * 帧编解码测试。
 *
 * 这是协议最外层 —— 所有 op 都经过它。信封层错了，后面全错，
 * 所以这里逐个字段盯：kind / id / op / seq 的形状。
 */
class TestFrames : public QObject {
  Q_OBJECT

 private slots:
  void parsesRequest();
  void parsesOkResponse();
  void parsesErrorResponse();
  void parsesEvent();
  void transientEventHasNullSeq();
  void rejectsGarbage();
  void rejectsMissingId();
  void rejectsMissingOp();
  void rejectsBadSeq();
  void rejectsOversizedFrame();
  void roundTripsRequest();
  void roundTripsEvent();
  void distinguishesNullSeqFromMissing();
};

void TestFrames::parsesRequest() {
  const auto r = parseFrame(R"({"v":1,"kind":"req","id":"r1","op":"conv.list","data":{"limit":10}})");
  QVERIFY(r.ok);
  QCOMPARE(r.frame.kind, FrameKind::Request);
  QCOMPARE(r.frame.id, QStringLiteral("r1"));
  QCOMPARE(r.frame.op, QStringLiteral("conv.list"));
  QCOMPARE(r.frame.data.value(QStringLiteral("limit")).toInt(), 10);
}

void TestFrames::parsesOkResponse() {
  const auto r = parseFrame(R"({"v":1,"kind":"res","id":"r1","ok":true,"data":{"conv":{"id":"c1"}}})");
  QVERIFY(r.ok);
  QCOMPARE(r.frame.kind, FrameKind::Response);
  QVERIFY(r.frame.ok);
  QCOMPARE(r.frame.result.value(QStringLiteral("conv")).toMap().value(QStringLiteral("id")).toString(),
           QStringLiteral("c1"));
}

void TestFrames::parsesErrorResponse() {
  const auto r =
      parseFrame(R"({"v":1,"kind":"res","id":"r1","ok":false,"error":{"code":"PERM_DENIED","message":"不行","details":{"action":"member"}}})");
  QVERIFY(r.ok);
  QVERIFY(!r.frame.ok);
  QCOMPARE(r.frame.error.code, QStringLiteral("PERM_DENIED"));
  QCOMPARE(r.frame.error.message, QStringLiteral("不行"));
  QVERIFY(r.frame.error.detailsJson.contains(QStringLiteral("member")));
}

void TestFrames::parsesEvent() {
  const auto r = parseFrame(
      R"({"v":1,"kind":"evt","type":"message.new","seq":42,"ts":1700000000000,"data":{"message":{"id":"m1"}}})");
  QVERIFY(r.ok);
  QCOMPARE(r.frame.kind, FrameKind::Event);
  QCOMPARE(r.frame.type, QStringLiteral("message.new"));
  QVERIFY(r.frame.seq.has_value());
  QCOMPARE(*r.frame.seq, 42LL);
  QCOMPARE(r.frame.ts, 1700000000000LL);
}

void TestFrames::transientEventHasNullSeq() {
  // 瞬时事件（打字 / 已读）seq 是 null（协议 §4.1）
  const auto r = parseFrame(R"({"v":1,"kind":"evt","type":"typing","seq":null,"ts":1,"data":{}})");
  QVERIFY(r.ok);
  QVERIFY(!r.frame.seq.has_value());
}

void TestFrames::rejectsGarbage() {
  QVERIFY(!parseFrame(QStringLiteral("not json")).ok);
  QVERIFY(!parseFrame(QStringLiteral("[1,2,3]")).ok);
  QVERIFY(!parseFrame(QStringLiteral("{}")).ok);
}

void TestFrames::rejectsMissingId() {
  const auto r = parseFrame(R"({"v":1,"kind":"req","op":"conv.list"})");
  QVERIFY(!r.ok);
  QVERIFY(r.error.contains(QStringLiteral("id")));
}

void TestFrames::rejectsMissingOp() {
  const auto r = parseFrame(R"({"v":1,"kind":"req","id":"r1"})");
  QVERIFY(!r.ok);
  QVERIFY(r.error.contains(QStringLiteral("op")));
}

void TestFrames::rejectsBadSeq() {
  // seq 必须是数字或 null —— 字符串是错的
  const auto r = parseFrame(R"({"v":1,"kind":"evt","type":"x","seq":"42","ts":1,"data":{}})");
  QVERIFY(!r.ok);
}

void TestFrames::rejectsOversizedFrame() {
  const QString huge(kMaxFrameBytes + 10, QLatin1Char('x'));
  const auto r = parseFrame(huge);
  QVERIFY(!r.ok);
  QVERIFY(r.error.contains(QStringLiteral("上限")));
}

void TestFrames::roundTripsRequest() {
  QVariantMap data;
  data.insert(QStringLiteral("conv_id"), QStringLiteral("c1"));
  const QString text = buildRequest(QStringLiteral("r9"), QStringLiteral("message.send"), data);
  const auto r = parseFrame(text);
  QVERIFY(r.ok);
  QCOMPARE(r.frame.id, QStringLiteral("r9"));
  QCOMPARE(r.frame.op, QStringLiteral("message.send"));
  QCOMPARE(r.frame.data.value(QStringLiteral("conv_id")).toString(), QStringLiteral("c1"));
}

void TestFrames::roundTripsEvent() {
  QVariantMap data;
  data.insert(QStringLiteral("x"), 1);
  const QString text =
      buildEvent(QStringLiteral("message.new"), std::optional<qint64>(7), 1234, data);
  const auto r = parseFrame(text);
  QVERIFY(r.ok);
  QCOMPARE(*r.frame.seq, 7LL);
  QCOMPARE(r.frame.ts, 1234LL);
  QCOMPARE(r.frame.eventData.value(QStringLiteral("x")).toInt(), 1);
}

void TestFrames::distinguishesNullSeqFromMissing() {
  // null = 瞬时事件（合法）；缺失 = 非法帧
  const auto withNull = parseFrame(R"({"v":1,"kind":"evt","type":"typing","seq":null,"ts":1,"data":{}})");
  QVERIFY(withNull.ok);
  QVERIFY(!withNull.frame.seq.has_value());
}

QTEST_GUILESS_MAIN(TestFrames)
#include "test_frames.moc"
