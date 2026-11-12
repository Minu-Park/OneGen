// Concurrent discovery must admit one of two producers with incompatible same-name imports.
#include "ProducerCoordinator.h"
#include <cassert>
#include <filesystem>
#include <iostream>
int main(int argc, char** argv) {
  assert(argc == 3);
  onegen::producer::Coordinator coordinator;
  assert(coordinator.set_producer_files({std::filesystem::canonical(argv[1]),
                                        std::filesystem::canonical(argv[2])}));
  const auto found = coordinator.discover();
  assert(found.completed && found.finished_producers == 2);
  assert(found.devices.size() == 1);
  assert(found.failures.size() == 1);
  assert(found.failures.front().find("refused") != std::string::npos);
  assert(found.failures.front().find("ConflictDependency.dll") != std::string::npos);
  assert(found.failures.front().find("..") == std::string::npos);
  assert(found.failures.front().find("Restart") != std::string::npos);
  assert(found.failures.front().find("Retry discovery explicitly") != std::string::npos);
  std::cout << "One conflicting producer admitted; the other refused before initialization.\n";
}
