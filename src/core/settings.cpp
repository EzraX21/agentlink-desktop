#include "settings.hpp"

#include <QDir>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>

namespace agentlink {

SettingsStore::SettingsStore(QObject* parent) : QObject(parent) {
  const QString base = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
  path_ = base + QStringLiteral("/client.ini");
}

void SettingsStore::load() {
  entries_.clear();
  if (!QFile::exists(path_)) {
    emit changed();
    return;
  }

  QSettings settings(path_, QSettings::IniFormat);
  const int count = settings.beginReadArray(QStringLiteral("servers"));
  for (int i = 0; i < count; ++i) {
    settings.setArrayIndex(i);
    ServerEntry entry;
    entry.address = settings.value(QStringLiteral("address")).toString();
    entry.rawAddress = settings.value(QStringLiteral("rawAddress")).toString();
    entry.username = settings.value(QStringLiteral("username")).toString();
    entry.displayName = settings.value(QStringLiteral("displayName")).toString();
    entry.accessToken = settings.value(QStringLiteral("accessToken")).toString();
    entry.refreshToken = settings.value(QStringLiteral("refreshToken")).toString();
    entry.active = settings.value(QStringLiteral("active")).toBool();
    if (entry.isValid()) entries_.push_back(entry);
  }
  settings.endArray();
  emit changed();
}

void SettingsStore::save() {
  QDir().mkpath(QFileInfo(path_).absolutePath());
  QSettings settings(path_, QSettings::IniFormat);
  settings.beginWriteArray(QStringLiteral("servers"), static_cast<int>(entries_.size()));
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    settings.setArrayIndex(static_cast<int>(i));
    const ServerEntry& e = entries_[i];
    settings.setValue(QStringLiteral("address"), e.address);
    settings.setValue(QStringLiteral("rawAddress"), e.rawAddress);
    settings.setValue(QStringLiteral("username"), e.username);
    settings.setValue(QStringLiteral("displayName"), e.displayName);
    settings.setValue(QStringLiteral("accessToken"), e.accessToken);
    settings.setValue(QStringLiteral("refreshToken"), e.refreshToken);
    settings.setValue(QStringLiteral("active"), e.active);
  }
  settings.endArray();
  settings.sync();
}

const ServerEntry* SettingsStore::activeEntry() const {
  for (const ServerEntry& e : entries_) {
    if (e.active) return &e;
  }
  return nullptr;
}

const ServerEntry* SettingsStore::findByAddress(const QString& address) const {
  for (const ServerEntry& e : entries_) {
    if (e.address == address) return &e;
  }
  return nullptr;
}

void SettingsStore::upsert(const ServerEntry& entry) {
  for (ServerEntry& e : entries_) {
    if (e.address == entry.address) {
      const bool wasActive = e.active;
      e = entry;
      // 覆盖时不要把"激活"标记弄丢：用户没说要换服务器。
      if (!entry.active && wasActive) e.active = true;
      save();
      emit changed();
      return;
    }
  }
  entries_.push_back(entry);
  save();
  emit changed();
}

void SettingsStore::remove(const QString& address) {
  const std::size_t before = entries_.size();
  for (auto it = entries_.begin(); it != entries_.end(); ++it) {
    if (it->address == address) {
      entries_.erase(it);
      break;
    }
  }
  if (entries_.size() != before) {
    save();
    emit changed();
  }
}

void SettingsStore::setActive(const QString& address) {
  bool anythingChanged = false;
  for (ServerEntry& e : entries_) {
    const bool shouldBeActive = e.address == address;
    if (e.active != shouldBeActive) {
      e.active = shouldBeActive;
      anythingChanged = true;
    }
  }
  if (anythingChanged) {
    save();
    emit changed();
  }
}

}  // namespace agentlink
