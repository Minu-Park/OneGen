#include "Internal/StageLog.h"
#include "Internal/ConnectTiming.h"
#include "Utility/Worker/WorkerLogForwarder.h"

#include <QCoreApplication>
#include <QList>
#include <algorithm>
#include <cassert>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

namespace {
struct Record { QtMsgType type; QString text; };
QList<Record> records;
void capture(QtMsgType type, const QMessageLogContext&, const QString& text) { records.append({type, text}); }
}

int main(int argc, char** argv)
{
  QCoreApplication application(argc, argv);
  std::stringbuf output;
  auto* original = std::clog.rdbuf(&output);
  onegen::stage_log("ready %d", 3);
  onegen::stage_log_warning("read failed rc=%d", -1006);
  const std::string long_detail(70000, 'x');
  onegen::stage_log_warning("detail=%s", long_detail.c_str());
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) threads.emplace_back([t] {
    for (int i = 0; i < 20; ++i) onegen::stage_log("thread=%d record=%d", t, i);
  });
  for (auto& thread : threads) thread.join();
  std::clog.rdbuf(original);
  const auto stage_output = output.str();
  assert(stage_output.find("[GenICam] [INFO] ready 3\n") != std::string::npos);
  assert(stage_output.find("[GenICam] [WARN] read failed rc=-1006\n") != std::string::npos);
  assert(stage_output.find("detail=" + long_detail + '\n') != std::string::npos);
  assert(std::count(stage_output.begin(), stage_output.end(), '\n') == 83);

  const auto handler = qInstallMessageHandler(capture);
  onegen::worker::WorkerLogForwarder forwarder(QStringLiteral("producer.cti"));
  const QByteArray fragmented = QString::fromUtf8("[GenICam] [WARN] read failed: 센서\r\n").toUtf8();
  for (const char byte : fragmented) forwarder.append(QByteArray(1, byte));
  forwarder.append("[GenICam] [INFO] ready\n[GenICam] [DEBUG] trace\n[GenICam] [ERROR] failed\n");
  forwarder.append("  vendor detail=<sdk>&\n[GenICam] [WARN] partial");
  assert(records.size() == 5);
  forwarder.finish();
  forwarder.finish();
  assert(records.size() == 6);
  assert(records[0].type == QtWarningMsg);
  assert(records[0].text == QString::fromUtf8("[GenICam] [WARN] read failed: 센서 cti=producer.cti"));
  assert(records[1].type == QtInfoMsg);
  assert(records[2].type == QtDebugMsg);
  assert(records[3].type == QtCriticalMsg);
  assert(records[4].type == QtWarningMsg);
  assert(records[4].text == QStringLiteral("[GenICam/Producer]   vendor detail=<sdk>& cti=producer.cti"));
  assert(records[5].type == QtWarningMsg);
  assert(records[5].text.endsWith(QStringLiteral("partial cti=producer.cti")));
  qInstallMessageHandler(handler);
  std::cout << "Stage serialization and worker diagnostic forwarding passed.\n";
}
