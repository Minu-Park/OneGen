#include "Internal/Gentl.h"

#include <cassert>
#include <string>

int main(int argc, char** argv)
{
  assert(argc == 2);
  onegen::Gentl producer;
  for (int attempt = 0; attempt < 3; ++attempt) {
    bool rejected = false;
    try {
      producer.load(argv[1]);
    } catch (const onegen::Error& error) {
      rejected = std::string(error.what()) == "GCInitLib failed (-1001)";
    }
    assert(rejected);
    assert(producer.lib == nullptr);
    assert(producer.GCGetLastError == nullptr);
    assert(producer.GCCloseLib == nullptr);
    producer.unload();
  }
}
