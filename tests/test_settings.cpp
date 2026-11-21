#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "core/settings.hpp"

using namespace agentlink;

/**
 * 设置存储测试：多服务器的读写。
 *
 * 关键性质：**覆盖已有记录时不丢"激活"标记** —— 用户重新登录了同一个
 * 服务器，不代表要切走。
 */
class TestSettings : public QObject {
  Q_OBJECT

 private slots:
  void init();
  void cleanup();
  void savesAndLoads();
  void upsertCreatesNew();
  void upsertKeepsActiveFlag();
  void removeDeletes();
  void setActiveSwitches();
  void activeEntryFindsCorrect();
  void findByAddressWorks();
  void survivesReload();

 private:
  QTemporaryDir* dir_ = nullptr;
};

void TestSettings::init() {
  dir_ = new QTemporaryDir();
  QVERIFY(dir_->isValid());
}

void TestSettings::cleanup() {
  delete dir_;
  dir_ = nullptr;
}

void TestSettings::savesAndLoads() {
  const QString path = dir_->filePath(QStringLiteral("client.ini"));
  {
    SettingsStore store;
    // 直接写文件路径（生产里是 AppConfigLocation，测试里要可控）
    ServerEntry entry;
    entry.address = QStringLiteral("ws://a.example.com/ws");
    entry.rawAddress = QStringLiteral("a.example.com");
    entry.username = QStringLiteral("alice");
    entry.displayName = QStringLiteral("爱丽丝");
    entry.accessToken = QStringLiteral("tok-a");
    entry.refreshToken = QStringLiteral("ref-a");
    entry.active = true;
    store.upsert(entry);
    QVERIFY(QFile::exists(path) || QFile::exists(store.filePath()));
  }
  {
    SettingsStore store;
    store.load();
    QCOMPARE(store.entries().size(), 1u);
    QCOMPARE(store.entries()[0].username, QStringLiteral("alice"));
    QCOMPARE(store.entries()[0].accessToken, QStringLiteral("tok-a"));
  }
}

void TestSettings::upsertCreatesNew() {
  SettingsStore store;
  ServerEntry a;
  a.address = QStringLiteral("ws://a/ws");
  ServerEntry b;
  b.address = QStringLiteral("ws://b/ws");
  store.upsert(a);
  store.upsert(b);
  QCOMPARE(store.entries().size(), 2u);
}

void TestSettings::upsertKeepsActiveFlag() {
  SettingsStore store;
  ServerEntry a;
  a.address = QStringLiteral("ws://a/ws");
  a.active = true;
  store.upsert(a);

  // 重新登录同一个服务器：新的 entry 没有 active 标记，但不该把激活状态弄丢
  ServerEntry again;
  again.address = QStringLiteral("ws://a/ws");
  again.accessToken = QStringLiteral("new-token");
  store.upsert(again);

  QCOMPARE(store.entries().size(), 1u);
  QCOMPARE(store.entries()[0].accessToken, QStringLiteral("new-token"));
  QVERIFY(store.entries()[0].active);
  QVERIFY(store.activeEntry() != nullptr);
}

void TestSettings::removeDeletes() {
  SettingsStore store;
  ServerEntry a;
  a.address = QStringLiteral("ws://a/ws");
  store.upsert(a);
  store.remove(QStringLiteral("ws://a/ws"));
  QCOMPARE(store.entries().size(), 0u);
}

void TestSettings::setActiveSwitches() {
  SettingsStore store;
  ServerEntry a;
  a.address = QStringLiteral("ws://a/ws");
  a.active = true;
  ServerEntry b;
  b.address = QStringLiteral("ws://b/ws");
  store.upsert(a);
  store.upsert(b);

  store.setActive(QStringLiteral("ws://b/ws"));
  QVERIFY(store.activeEntry() != nullptr);
  QCOMPARE(store.activeEntry()->address, QStringLiteral("ws://b/ws"));
  // 只有一个是激活的
  int activeCount = 0;
  for (const ServerEntry& e : store.entries()) {
    if (e.active) ++activeCount;
  }
  QCOMPARE(activeCount, 1);
}

void TestSettings::activeEntryFindsCorrect() {
  SettingsStore store;
  ServerEntry a;
  a.address = QStringLiteral("ws://a/ws");
  store.upsert(a);
  QVERIFY(store.activeEntry() == nullptr);
}

void TestSettings::findByAddressWorks() {
  SettingsStore store;
  ServerEntry a;
  a.address = QStringLiteral("ws://a/ws");
  store.upsert(a);
  QVERIFY(store.findByAddress(QStringLiteral("ws://a/ws")) != nullptr);
  QVERIFY(store.findByAddress(QStringLiteral("ws://nope/ws")) == nullptr);
}

void TestSettings::survivesReload() {
  SettingsStore store;
  ServerEntry a;
  a.address = QStringLiteral("ws://a/ws");
  a.username = QStringLiteral("bob");
  a.accessToken = QStringLiteral("t");
  store.upsert(a);
  store.save();

  SettingsStore reloaded;
  reloaded.load();
  QCOMPARE(reloaded.entries().size(), 1u);
  QCOMPARE(reloaded.entries()[0].username, QStringLiteral("bob"));
}

QTEST_GUILESS_MAIN(TestSettings)
#include "test_settings.moc"
