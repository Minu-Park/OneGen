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
