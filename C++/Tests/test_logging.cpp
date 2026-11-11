#include "Internal/StageLog.h"
#include "Internal/ConnectTiming.h"
#include "Utility/Qt/OneGenLogSink.h"

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

  // Without a sink, complete typed records go to the standard diagnostic stream.
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

  // With the Qt sink, severity is preserved and nothing reaches the standard stream.
  const auto handler = qInstallMessageHandler(capture);
  onegen::qt::installLogSink();
  std::stringbuf silent;
  original = std::clog.rdbuf(&silent);
  onegen::stage_log_warning("read failed: %s", "\xec\x84\xbc\xec\x84\x9c");
  onegen::stage_log("ready");
  onegen::stage_log_debug("trace");
  assert(records.size() == 3);
  assert(records[0].type == QtWarningMsg);
  assert(records[0].text == QString::fromUtf8("[GenICam] read failed: \xec\x84\xbc\xec\x84\x9c"));
  assert(records[1].type == QtInfoMsg && records[1].text == QStringLiteral("[GenICam] ready"));
  assert(records[2].type == QtDebugMsg);

  // Discovery threads report scan detail at Debug; the scan owner reports outcomes.
  std::thread([] {
    const onegen::StageLogDiscoveryScope quiet;
    onegen::stage_log_warning("init failed rc=%d", -1001);
    onegen::stage_log("interface scan completed");
  }).join();
  assert(records.size() == 5);
  assert(records[3].type == QtDebugMsg && records[3].text.contains(QStringLiteral("rc=-1001")));
  assert(records[4].type == QtDebugMsg);
  // The scope is per thread and restored on exit.
  onegen::stage_log_warning("acquisition failed");
  assert(records.back().type == QtWarningMsg);

  onegen::set_log_sink(nullptr);
  std::clog.rdbuf(original);
  assert(silent.str().empty());
  qInstallMessageHandler(handler);
  std::cout << "Stage serialization and in-process diagnostic routing passed.\n";
}
