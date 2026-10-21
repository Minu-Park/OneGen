#include "OneGenWorkerClient.h"
#include "Utility/Worker/WorkerException.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <atomic>
#include <iostream>
#include <initializer_list>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
std::mutex logMutex;
std::vector<std::string> logs;
void capture(QtMsgType, const QMessageLogContext&, const QString& message) {
  std::lock_guard lock(logMutex);
  logs.push_back(message.toStdString());
}
void require(bool condition, const std::string& detail) {
  if (!condition) throw std::runtime_error(detail);
}
bool logged(std::initializer_list<std::string> fragments) {
  std::lock_guard lock(logMutex);
  for (const auto& line : logs) {
    bool matches = true;
    for (const auto& fragment : fragments)
      if (line.find(fragment) == std::string::npos) { matches = false; break; }
    if (matches) return true;
  }
  return false;
}
void requestFailureSeverity() {
  std::ostringstream output;
  auto* previous = std::clog.rdbuf(output.rdbuf());
  std::string expected, unknown;
  try {
    try { throw std::runtime_error("request rejected"); }
    catch (...) { expected = onegen::worker::reportRequestException("worker request failed: id=3"); }
    try { throw 7; }
    catch (...) { unknown = onegen::worker::reportRequestException("worker single acquisition failed: id=4"); }
  } catch (...) { std::clog.rdbuf(previous); throw; }
  std::clog.rdbuf(previous);
  require(expected == "request rejected" && unknown == "Unknown C++ exception.", "request reply detail changed");
  require(output.str() == "[GenICam] [INFO] worker request failed: id=3 detail=request rejected\n"
                          "[GenICam] [WARN] worker single acquisition failed: id=4 detail=Unknown C++ exception.\n",
          "request failure severity or context changed: " + output.str());
}
void rejectedProcess(const QString& program, const QStringList& args, const QByteArray& expected) {
  QProcess process;
  process.start(program, args);
  require(process.waitForStarted(5000), "diagnostic test peer did not start: " + process.errorString().toStdString());
  require(process.waitForFinished(15000), "diagnostic test peer timed out");
  const auto diagnostic = process.readAllStandardError();
  require(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 1,
          "failure must return 1 without abort: " + diagnostic.toStdString());
  require(diagnostic.contains(expected), "missing diagnostic: " + expected.toStdString() + " actual=" + diagnostic.toStdString());
  require(process.readAllStandardOutput().isEmpty(), "diagnostics polluted protocol stdout");
}
}

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  try {
    require(argc == 4, "expected peer, worker and failing producer paths");
    requestFailureSeverity();
    const QString peer = QString::fromLocal8Bit(argv[1]);
    QTemporaryDir directory;
    require(directory.isValid(), "temporary diagnostic directory unavailable");
    rejectedProcess(peer, {"--inactive-producer", directory.filePath("missing-worker"), argv[3]}, "Worker start failed:");
    rejectedProcess(peer, {"--inactive-producer", peer, argv[3]}, "attempt=1 alive=1 inactive=0 observed=request unexpectedly succeeded");
    rejectedProcess(QString::fromLocal8Bit(argv[2]), {}, "worker fatal error:");

    const auto previous = qInstallMessageHandler(capture);
    try {
      for (const auto name : {"FatalDiagnostics.cti", "MalformedDiagnostics.cti"}) {
        onegen::worker::Connection connection(argv[1], name);
        std::string failure;
        try { connection.request(onegen::worker::Operation::Probe, {}, std::chrono::seconds{2}); }
        catch (const std::exception& error) { failure = error.what(); }
        const bool fatal = std::string(name) == "FatalDiagnostics.cti";
        require(failure.find(fatal ? "0x00000047" : "message is too large") != std::string::npos,
                "process/protocol failure lost its identity: " + failure);
        if (fatal) require(failure.find("[GenICam] [ERROR] fixture fatal detail") != std::string::npos,
                           "final tagged diagnostic was discarded: " + failure);
        require(!connection.alive(), "failed worker remained alive");
      }
      for (const bool unknown : {false, true}) {
        onegen::worker::Connection connection(argv[1], "ErrorDiagnostics.cti");
        connection.set_error_callback([unknown](std::string_view) {
          if (unknown) throw 7;
          throw std::runtime_error("error callback detail");
        });
        std::string failure;
        try { connection.request(onegen::worker::Operation::Probe, {}, std::chrono::seconds{2}); }
        catch (const std::exception& error) { failure = error.what(); }
        require(failure.find("message processing") != std::string::npos
                    && failure.find(unknown ? "Unknown C++ exception." : "error callback detail") != std::string::npos
                    && failure.find("cti=ErrorDiagnostics.cti") != std::string::npos,
                "error callback failure context missing: " + failure);
        require(!connection.alive(), "failed error callback did not close connection");
      }
      {
        onegen::worker::Connection connection(argv[1], "InvalidateDiagnostics.cti");
        connection.set_invalidate_callback([](int, std::string_view) { throw 9; });
        std::string failure;
        try { connection.request(onegen::worker::Operation::Probe, {}, std::chrono::seconds{2}); }
        catch (const std::exception& error) { failure = error.what(); }
        require(failure.find("Unknown C++ exception.") != std::string::npos && !connection.alive(),
                "invalidate callback escaped its IO boundary: " + failure);
      }
      for (const bool unknown : {false, true}) {
        { std::lock_guard lock(logMutex); logs.clear(); }
        onegen::worker::Connection connection(argv[1], "FrameDiagnostics.cti");
        std::atomic<int> frames{0};
        connection.set_frame_callback([&](const auto&) {
          ++frames;
          if (unknown) throw 11;
          throw std::runtime_error("frame callback detail");
        });
        for (int count = 1; count <= 2; ++count) {
          connection.request(onegen::worker::Operation::Probe);
          QElapsedTimer timer; timer.start();
          while (frames < count && timer.elapsed() < 3000) QThread::msleep(1);
          require(frames == count && connection.alive(), "frame callback failure broke continued delivery");
        }
        connection.close();
        require(logged({"frame consumer failed:", "cti=FrameDiagnostics.cti",
                        unknown ? "detail=Unknown C++ exception." : "detail=frame callback detail"}),
                "frame callback diagnostic record was lost");
      }
    } catch (...) { qInstallMessageHandler(previous); throw; }
    qInstallMessageHandler(previous);
    std::cout << "worker exception boundaries and failure diagnostics passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "test_worker_errors failed: " << error.what() << std::endl;
    return 1;
  }
}
