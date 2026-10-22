#include "DevicePlugin/DevicePlugin.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QPluginLoader>
#include <QTemporaryDir>
#include <QTest>
#include <QScopeGuard>
#include <algorithm>
#include <future>

namespace {
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
  void firstCameraThenTerminalInfoLog() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString fast = temporary.filePath("Fast.cti");
    const QString slow = temporary.filePath("Heartbeat.cti");
    const QString crash = temporary.filePath("CrashProbe.cti");
    for (const auto& path : {fast, slow, crash}) { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); }
    qputenv("ONEGEN_WORKER_PATH", QByteArray(ONEGEN_TEST_PEER));
    QPluginLoader loader(QString::fromUtf8(ONEGEN_TEST_PLUGIN));
    auto* object = loader.instance();
    QVERIFY2(object, qPrintable(loader.errorString()));
    auto* plugin = qobject_cast<IDevicePlugin*>(object);
    auto* producers = qobject_cast<IDevicePluginGenTLProducers*>(object);
    auto* observer = qobject_cast<IDevicePluginGenTLDiscoveryStatus*>(object);
    QVERIFY(plugin && producers && observer);
    QString error;
    QVERIFY(producers->setGenTLProducerFiles({fast, slow, crash}, &error));
    QCOMPARE(observer->genTLDiscoveryStatus().phase, GenTLDiscoveryPhase::Waiting);
    QVERIFY(!QFile::exists(fast + ".starts"));
    QVERIFY(!QFile::exists(slow + ".starts"));
    logs.clear();
    const auto previous = qInstallMessageHandler(capture);
    const auto restoreHandler = qScopeGuard([previous] { qInstallMessageHandler(previous); });
    auto search = std::async(std::launch::async, [&] {
      QVariantMap data;
      const bool ready = plugin->discoverDevices(&data, &error);
      return qMakePair(ready, data);
    });
    QTRY_VERIFY_WITH_TIMEOUT(observer->genTLDiscoveryStatus().detail.contains("/ 3"), 2000);
    QVERIFY(search.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    const auto result = search.get();
    QVERIFY2(result.first, qPrintable(error));
    QVERIFY(!result.second.value("devices").toList().isEmpty());
    QCOMPARE(observer->genTLDiscoveryStatus().phase, GenTLDiscoveryPhase::Running);
    QVERIFY(!hasInfo("camera search completed:"));
    QTRY_COMPARE_WITH_TIMEOUT(observer->genTLDiscoveryStatus().phase, GenTLDiscoveryPhase::Partial, 10000);
    QVERIFY(hasInfo("camera search completed:"));
    QVERIFY(!hasInfo("producer search result:"));
    QVERIFY(hasInfo("producer search failed:"));
    QVERIFY(hasInfo("CrashProbe.cti"));
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
        QVERIFY2(item.first != QtWarningMsg && item.first != QtCriticalMsg, qPrintable(item.second));
        if (item.first == QtInfoMsg) {
          QVERIFY2(!item.second.contains("worker process"), qPrintable(item.second));
          QVERIFY2(!item.second.contains("discovery diagnostic:"), qPrintable(item.second));
          QVERIFY2(!item.second.contains(" -> "), qPrintable(item.second));
        }
      }
    }
    QVERIFY(loader.unload());
  }
  void emptyAndFailedScans_data() {
    QTest::addColumn<QString>("producer");
    QTest::addColumn<int>("phase");
    QTest::newRow("no-producers") << QString{} << int(GenTLDiscoveryPhase::Succeeded);
    QTest::newRow("no-camera") << QString("EmptyBoard") << int(GenTLDiscoveryPhase::Succeeded);
    QTest::newRow("all-failed") << QString("ProbeFail") << int(GenTLDiscoveryPhase::Failed);
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
    qputenv("ONEGEN_WORKER_PATH", QByteArray(ONEGEN_TEST_PEER));
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
    if (phase == int(GenTLDiscoveryPhase::Failed)) QVERIFY(hasInfo("TLOpenInterface: test failure"));
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
