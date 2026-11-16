#include "DevicePlugin/AbstractSourceController.h"
#include "DevicePlugin/DeviceConnectionControl.h"
#include "DevicePlugin/DevicePlugin.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QPluginLoader>
#include <QTemporaryDir>
#include <QTest>
#include <QScopeGuard>
#include <algorithm>
#include <atomic>
#include <functional>
#include <future>
#include <memory>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
class TestResourceClaim final : public IDevicePluginResourceClaim {
public:
  TestResourceClaim(QString key, std::function<void()> release)
      : key_(std::move(key)), release_(std::move(release)) {}
  ~TestResourceClaim() override { release(); }
  QString key() const override { return key_; }
  void release() override {
    if (released_.exchange(true)) return;
    release_();
  }

private:
  QString key_;
  std::function<void()> release_;
  std::atomic<bool> released_{false};
};

/** @brief Host claim stub used to prove the plugin's per-camera claim path. */
class TestResourceClaims final : public IDevicePluginResourceClaims {
public:
  void hold(const QString& key) {
    QMutexLocker lock(&mutex_);
    ++held_[key];
  }
  void drop(const QString& key) {
    QMutexLocker lock(&mutex_);
    if (--held_[key] <= 0) held_.remove(key);
  }
  std::shared_ptr<IDevicePluginResourceClaim> claimResource(const QString& key,
      QString* error) override {
    {
      QMutexLocker lock(&mutex_);
      if (held_.value(key) > 0) {
        if (error) *error = QStringLiteral("Hardware resource '%1' is in use by another session.")
                                .arg(key);
        return {};
      }
      ++held_[key];
    }
    return std::make_shared<TestResourceClaim>(key, [this, key] { drop(key); });
  }

private:
  QMutex mutex_;
  QHash<QString, int> held_;
};

QMutex log_mutex;
QList<QPair<QtMsgType, QString>> logs;
void capture(QtMsgType type, const QMessageLogContext&, const QString& text) {
  QMutexLocker lock(&log_mutex);
  logs.append({type, text});
}
bool hasInfo(const QString& text) {
  QMutexLocker lock(&log_mutex);
  return std::any_of(logs.cbegin(), logs.cend(), [&](const auto& item) {
    return item.first == QtInfoMsg && item.second.contains(text);
  });
}
}

class PluginDiscoveryTests : public QObject {
  Q_OBJECT
private slots:
  void shutdownReleasesProducerBeforeReopen() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString cti = temporary.filePath("Retired.cti");
    QVERIFY(QFile::copy(QString::fromUtf8(ONEGEN_TEST_PRODUCER), cti));
    const auto mapped = [&] {
#ifdef _WIN32
      return GetModuleHandleW(cti.toStdWString().c_str()) != nullptr;
#else
      void* module = dlopen(QFile::encodeName(cti).constData(), RTLD_LAZY | RTLD_NOLOAD);
      if (module) dlclose(module);
      return module != nullptr;
#endif
    };
    for (int attempt = 0; attempt < 3; ++attempt) {
      QVERIFY(!mapped());
      QPluginLoader loader;
      loader.setLoadHints(QLibrary::LoadHints{});
      loader.setFileName(QString::fromUtf8(ONEGEN_TEST_PLUGIN));
      auto* object = loader.instance();
      QVERIFY2(object, qPrintable(loader.errorString()));
      auto* plugin = qobject_cast<IDevicePlugin*>(object);
      auto* producers = qobject_cast<IDevicePluginGenTLProducers*>(object);
      QVERIFY(plugin && producers);
      QString error;
      QVERIFY2(producers->setGenTLProducerFiles({cti}, &error), qPrintable(error));
      QVariantMap data;
      QVERIFY2(plugin->discoverDevices(&data, &error), qPrintable(error));
      QVERIFY(mapped());
      {
        auto session = plugin->createSession(data, &error);
        QVERIFY2(session != nullptr, qPrintable(error));
        QWidget controls;
        QVERIFY(!session->createDockWidgets(&controls).empty());
        session.reset();
      }
      plugin->shutdown();
      QVERIFY(!mapped());
      QVERIFY(loader.unload());
    }
  }
  void cameraThenTerminalInfoLog() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    // Producers load in this process: a real synthetic producer and an unloadable file.
    const QString fast = temporary.filePath("Fast.cti");
    const QString broken = temporary.filePath("Broken.cti");
    QVERIFY(QFile::copy(QString::fromUtf8(ONEGEN_TEST_PRODUCER), fast));
    { QFile file(broken); QVERIFY(file.open(QIODevice::WriteOnly)); }
    QPluginLoader loader(QString::fromUtf8(ONEGEN_TEST_PLUGIN));
    auto* object = loader.instance();
    QVERIFY2(object, qPrintable(loader.errorString()));
    auto* plugin = qobject_cast<IDevicePlugin*>(object);
    auto* producers = qobject_cast<IDevicePluginGenTLProducers*>(object);
    auto* observer = qobject_cast<IDevicePluginGenTLDiscoveryStatus*>(object);
    QVERIFY(plugin && producers && observer);
    QString error;
    QVERIFY(producers->setGenTLProducerFiles({fast, broken}, &error));
    QCOMPARE(observer->genTLDiscoveryStatus().phase, GenTLDiscoveryPhase::Waiting);
    logs.clear();
    const auto previous = qInstallMessageHandler(capture);
    const auto restoreHandler = qScopeGuard([previous] { qInstallMessageHandler(previous); });
    QVariantMap data;
    QVERIFY2(plugin->discoverDevices(&data, &error), qPrintable(error));
    QVERIFY(!data.value("devices").toList().isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(observer->genTLDiscoveryStatus().phase, GenTLDiscoveryPhase::Partial, 10000);
    QVERIFY(hasInfo("camera search completed:"));
    QVERIFY(hasInfo("producer search failed:"));
    QVERIFY(hasInfo("Broken.cti"));
    // The same completed scan is reused when a second session opens.
    QVariantMap replay;
    QVERIFY(plugin->discoverDevices(&replay, &error));
    QVERIFY(!replay.value("devices").toList().isEmpty());
    plugin->shutdown();
    qInstallMessageHandler(previous);
    {
      QMutexLocker lock(&log_mutex);
      QCOMPARE(std::count_if(logs.cbegin(), logs.cend(), [](const auto& item) {
        return item.first == QtInfoMsg;
      }), 3); // One start, one completion, one failed CTI; cached opens add no records.
      for (const auto& item : logs) {
        // Producer load and scan detail stay at Debug; only the scan owner reports outcomes.
        QVERIFY2(item.first != QtWarningMsg && item.first != QtCriticalMsg, qPrintable(item.second));
        if (item.first == QtInfoMsg) QVERIFY2(!item.second.contains(" -> "), qPrintable(item.second));
      }
    }
    QVERIFY(loader.unload());
  }
  void cameraClaimDenialIsAPreEffectConflict() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString cti = temporary.filePath("Claim.cti");
    QVERIFY(QFile::copy(QString::fromUtf8(ONEGEN_TEST_PRODUCER), cti));
    qputenv("ONEGEN_SYNTHETIC_DEVICES", "1");
    qputenv("ONEGEN_SYNTHETIC_INTERFACE_TYPE", "GEV");
    const auto restoreEnvironment = qScopeGuard([] {
      qunsetenv("ONEGEN_SYNTHETIC_DEVICES");
      qunsetenv("ONEGEN_SYNTHETIC_INTERFACE_TYPE");
    });

    QPluginLoader loader(QString::fromUtf8(ONEGEN_TEST_PLUGIN));
    auto* object = loader.instance();
    QVERIFY2(object, qPrintable(loader.errorString()));
    auto* plugin = qobject_cast<IDevicePlugin*>(object);
    auto* producers = qobject_cast<IDevicePluginGenTLProducers*>(object);
    auto* receiver = dynamic_cast<IDevicePluginResourceClaimsReceiver*>(object);
    QVERIFY(plugin && producers && receiver);
    TestResourceClaims claims;
    receiver->setResourceClaims(&claims);
    QString error;
    QVERIFY2(producers->setGenTLProducerFiles({cti}, &error), qPrintable(error));
    QVariantMap discovery;
    QVERIFY2(plugin->discoverDevices(&discovery, &error), qPrintable(error));
    auto session = plugin->createSession(discovery, &error);
    QVERIFY2(session != nullptr, qPrintable(error));
    auto* connection = dynamic_cast<IDeviceConnectionControl*>(session->sourceController());
    QVERIFY(connection);

    const auto requestTargets = [&]() -> DeviceConnectionTargets {
      auto promise = std::make_shared<std::promise<DeviceConnectionTargets>>();
      auto future = promise->get_future();
      const auto admitted = connection->requestConnectionTargets([promise](DeviceConnectionTargets value) {
        promise->set_value(std::move(value));
      });
      if (admitted.error != DeviceConnectionError::None)
        return {{admitted.error, admitted.message}, {}};
      if (future.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
        return {{DeviceConnectionError::Timeout, "Target observation timed out."}, {}};
      return future.get();
    };
    const auto connect = [&](const std::string& target) {
      auto promise = std::make_shared<std::promise<DeviceConnectionResult>>();
      auto future = promise->get_future();
      const auto admitted = connection->requestConnect(target, [promise](DeviceConnectionResult value) {
        promise->set_value(std::move(value));
      });
      if (admitted.error != DeviceConnectionError::None) return admitted;
      if (future.wait_for(std::chrono::seconds(30)) != std::future_status::ready)
        return DeviceConnectionResult{DeviceConnectionError::Timeout, "Connect timed out."};
      return future.get();
    };

    const auto targets = requestTargets();
    const auto camera = std::find_if(targets.targets.begin(), targets.targets.end(),
        [](const auto& target) { return target.kind == DeviceConnectionTargetKind::Device; });
    QVERIFY(camera != targets.targets.end());
    const QString key = QStringLiteral("camera:synth-0001");
    claims.hold(key);
    const auto refused = connect(camera->targetId);
    QCOMPARE(refused.error, DeviceConnectionError::Conflict);
    QCOMPARE(refused.effect, DeviceConnectionEffect::None);
    QVERIFY(refused.message.find("in use") != std::string::npos);
    QVERIFY(!refused.state || !refused.state->connected);
    claims.drop(key);
    const auto accepted = connect(camera->targetId);
    QVERIFY2(accepted.error == DeviceConnectionError::None, accepted.message.c_str());
    QVERIFY(accepted.state && accepted.state->connected);

    session.reset();
    plugin->shutdown();
    QVERIFY(loader.unload());
  }

  void emptyAndFailedScans_data() {
    QTest::addColumn<QString>("producer");
    QTest::addColumn<int>("phase");
    QTest::newRow("no-producers") << QString{} << int(GenTLDiscoveryPhase::Succeeded);
    QTest::newRow("all-failed") << QString("Broken") << int(GenTLDiscoveryPhase::Failed);
  }
  void emptyAndFailedScans() {
    QFETCH(QString, producer);
    QFETCH(int, phase);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QStringList files;
    if (!producer.isEmpty()) {
      files.append(temporary.filePath(producer + ".cti"));
      QFile file(files.front());
      QVERIFY(file.open(QIODevice::WriteOnly));
    }
    QPluginLoader loader(QString::fromUtf8(ONEGEN_TEST_PLUGIN));
    auto* object = loader.instance();
    QVERIFY2(object, qPrintable(loader.errorString()));
    auto* plugin = qobject_cast<IDevicePlugin*>(object);
    auto* producers = qobject_cast<IDevicePluginGenTLProducers*>(object);
    auto* observer = qobject_cast<IDevicePluginGenTLDiscoveryStatus*>(object);
    QVERIFY(plugin && producers && observer);
    QString error;
    QVERIFY(producers->setGenTLProducerFiles(files, &error));
    logs.clear();
    const auto previous = qInstallMessageHandler(capture);
    const auto restoreHandler = qScopeGuard([previous] { qInstallMessageHandler(previous); });
    QVariantMap data;
    QVERIFY(plugin->discoverDevices(&data, &error));
    QVERIFY(data.value("devices").toList().isEmpty());
    QCOMPARE(int(observer->genTLDiscoveryStatus().phase), phase);
    QVERIFY(hasInfo("camera search completed:"));
    if (phase == int(GenTLDiscoveryPhase::Failed)) QVERIFY(hasInfo("Producer inactive"));
    auto session = plugin->createSession(data, &error);
    QVERIFY2(session, qPrintable(error));
    session.reset();
    plugin->shutdown();
    qInstallMessageHandler(previous);
    {
      QMutexLocker lock(&log_mutex);
      for (const auto& item : logs)
        QVERIFY2(item.first != QtWarningMsg && item.first != QtCriticalMsg, qPrintable(item.second));
    }
    QVERIFY(loader.unload());
  }
};
QTEST_MAIN(PluginDiscoveryTests)
#include "test_plugin_discovery.moc"
