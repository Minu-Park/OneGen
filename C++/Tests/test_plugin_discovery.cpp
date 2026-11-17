#include "DeviceRuntime/Contract/AbstractSourceController.h"
#include "DeviceRuntime/Contract/DeviceConnectionControl.h"
#include "DeviceRuntime/Contract/DeviceRuntime.h"
#include "DeviceRuntime/Contract/DevicePluginSession.h"
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
      auto* plugin = qobject_cast<IDeviceRuntime*>(object);
      auto* producers = qobject_cast<IDevicePluginGenTLProducers*>(object);
      QVERIFY(plugin && producers);
      QString error;
      QVERIFY2(producers->setGenTLProducerFiles({cti}, &error), qPrintable(error));
      QVERIFY2(plugin->discoverDevices(&error), qPrintable(error));
      QVERIFY(mapped());
      {
        auto session = plugin->createSession({}, &error);
        QVERIFY2(session != nullptr, qPrintable(error));
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
    auto* plugin = qobject_cast<IDeviceRuntime*>(object);
    auto* producers = qobject_cast<IDevicePluginGenTLProducers*>(object);
    auto* observer = qobject_cast<IDevicePluginGenTLDiscoveryStatus*>(object);
    QVERIFY(plugin && producers && observer);
    QString error;
    QVERIFY(producers->setGenTLProducerFiles({fast, broken}, &error));
    QCOMPARE(observer->genTLDiscoveryStatus().phase, GenTLDiscoveryPhase::Waiting);
    logs.clear();
    const auto previous = qInstallMessageHandler(capture);
    const auto restoreHandler = qScopeGuard([previous] { qInstallMessageHandler(previous); });
    QVERIFY2(plugin->discoverDevices(&error), qPrintable(error));
    QTRY_COMPARE_WITH_TIMEOUT(observer->genTLDiscoveryStatus().phase, GenTLDiscoveryPhase::Partial, 10000);
    QVERIFY(hasInfo("camera search completed:"));
    QVERIFY(hasInfo("producer search failed:"));
    QVERIFY(hasInfo("Broken.cti"));
    // Repeating discovery reuses the retained result without another producer search.
    QVERIFY(plugin->discoverDevices(&error));
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
    auto* plugin = qobject_cast<IDeviceRuntime*>(object);
    auto* producers = qobject_cast<IDevicePluginGenTLProducers*>(object);
    auto* receiver = dynamic_cast<IDevicePluginResourceClaimsReceiver*>(object);
    QVERIFY(plugin && producers && receiver);
    TestResourceClaims claims;
    receiver->setResourceClaims(&claims);
    QString error;
    QVERIFY2(producers->setGenTLProducerFiles({cti}, &error), qPrintable(error));
    QVERIFY2(plugin->discoverDevices(&error), qPrintable(error));
    auto session = plugin->createSession({}, &error);
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

  void genTLPathsRefreshSessionWithoutWidget() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString cti = temporary.filePath("Refresh.cti");
    QVERIFY(QFile::copy(QString::fromUtf8(ONEGEN_TEST_PRODUCER), cti));
    qputenv("ONEGEN_SYNTHETIC_DEVICES", "2");
    qputenv("ONEGEN_SYNTHETIC_INTERFACE_TYPE", "CXP");
    const auto restoreEnvironment = qScopeGuard([] {
      qunsetenv("ONEGEN_SYNTHETIC_DEVICES");
      qunsetenv("ONEGEN_SYNTHETIC_INTERFACE_TYPE");
      qunsetenv("ONEGEN_SYNTHETIC_HIDDEN_DEVICES");
    });

    QPluginLoader loader(QString::fromUtf8(ONEGEN_TEST_PLUGIN));
    auto* object = loader.instance();
    QVERIFY2(object, qPrintable(loader.errorString()));
    auto* plugin = qobject_cast<IDeviceRuntime*>(object);
    auto* producers = qobject_cast<IDevicePluginGenTLProducers*>(object);
    auto* paths = qobject_cast<IDevicePluginGenTLPaths*>(object);
    auto* retry = qobject_cast<IDevicePluginGenTLRetry*>(object);
    auto* claimsReceiver = dynamic_cast<IDevicePluginResourceClaimsReceiver*>(object);
    QVERIFY(plugin && producers && paths && retry && claimsReceiver);
    TestResourceClaims claims;
    claimsReceiver->setResourceClaims(&claims);
    QString error;
    QVERIFY2(producers->setGenTLProducerFiles({cti}, &error), qPrintable(error));
    QVERIFY2(plugin->discoverDevices(&error), qPrintable(error));
    auto session = plugin->createSession({}, &error);
    QVERIFY2(session != nullptr, qPrintable(error));
    auto* connection = dynamic_cast<IDeviceConnectionControl*>(session->sourceController());
    QVERIFY(connection);

    const auto targets = [&]() -> DeviceConnectionTargets {
      DeviceConnectionTargets value;
      const auto admitted = connection->requestConnectionTargets(
          [&value](DeviceConnectionTargets result) { value = std::move(result); });
      if (admitted.error != DeviceConnectionError::None)
        return {{admitted.error, admitted.message}, {}};
      return value;
    };
    const auto deviceCount = [&]() {
      const auto discovered = targets();
      return std::count_if(discovered.targets.cbegin(), discovered.targets.cend(),
          [](const auto& target) { return target.kind == DeviceConnectionTargetKind::Device; });
    };
    QVERIFY(deviceCount() > 0);
    QVERIFY(QTest::qWaitFor([&] { return paths->genTLPathChangeBlockReason().isEmpty(); }, 10000));

    // No widget is registered; the host order must reach the live session through its controller.
    qputenv("ONEGEN_SYNTHETIC_HIDDEN_DEVICES", "0");
    QVERIFY2(paths->resetGenTLSearch(&error), qPrintable(error));
    QVERIFY2(producers->setGenTLProducerFiles({cti}, &error), qPrintable(error));
    QVERIFY2(producers->startGenTLDiscovery(&error), qPrintable(error));
    paths->rescanGenTLPaths();
    QVERIFY(QTest::qWaitFor([&] { return deviceCount() == 1
        && paths->genTLPathChangeBlockReason().isEmpty(); }, 30000));
    qunsetenv("ONEGEN_SYNTHETIC_HIDDEN_DEVICES");
    QVERIFY2(retry->retryGenTLDiscovery(&error), qPrintable(error));
    QVERIFY(QTest::qWaitFor([&] { return deviceCount() > 1
        && paths->genTLPathChangeBlockReason().isEmpty(); }, 30000));

    const auto discovered = targets();
    const auto camera = std::find_if(discovered.targets.cbegin(), discovered.targets.cend(),
        [](const auto& target) { return target.kind == DeviceConnectionTargetKind::Device; });
    QVERIFY(camera != discovered.targets.cend());
    auto promise = std::make_shared<std::promise<DeviceConnectionResult>>();
    auto future = promise->get_future();
    const auto admitted = connection->requestConnect(camera->targetId,
        [promise](DeviceConnectionResult result) { promise->set_value(std::move(result)); });
    QVERIFY2(admitted.error == DeviceConnectionError::None, admitted.message.c_str());
    QCOMPARE(future.wait_for(std::chrono::seconds(30)), std::future_status::ready);
    const auto connected = future.get();
    QVERIFY2(connected.error == DeviceConnectionError::None, connected.message.c_str());
    const auto reason = paths->genTLPathChangeBlockReason();
    QVERIFY(reason.contains("Disconnect"));
    QVERIFY(!retry->retryGenTLDiscovery(&error));
    QCOMPARE(error, reason);

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
    auto* plugin = qobject_cast<IDeviceRuntime*>(object);
    auto* producers = qobject_cast<IDevicePluginGenTLProducers*>(object);
    auto* observer = qobject_cast<IDevicePluginGenTLDiscoveryStatus*>(object);
    QVERIFY(plugin && producers && observer);
    QString error;
    QVERIFY(producers->setGenTLProducerFiles(files, &error));
    logs.clear();
    const auto previous = qInstallMessageHandler(capture);
    const auto restoreHandler = qScopeGuard([previous] { qInstallMessageHandler(previous); });
    QVERIFY(plugin->discoverDevices(&error));
    QCOMPARE(int(observer->genTLDiscoveryStatus().phase), phase);
    QVERIFY(hasInfo("camera search completed:"));
    if (phase == int(GenTLDiscoveryPhase::Failed)) QVERIFY(hasInfo("Producer inactive"));
    auto session = plugin->createSession({}, &error);
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
QTEST_GUILESS_MAIN(PluginDiscoveryTests)
#include "test_plugin_discovery.moc"
