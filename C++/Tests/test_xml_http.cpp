#include "Internal/XmlHttp.h"
#include <cassert>
#include <iostream>

int main(int argc, char** argv)
{
  assert(argc == 3);
  const bool success = std::string_view(argv[2]) == "ok";
  try {
    const auto body = onegen::fetch_xml_http(argv[1]);
    assert(success);
    assert(std::string(body.begin(), body.end()) == "<xml/>");
  } catch (const std::exception& error) {
    std::cout << error.what() << '\n';
    assert(!success);
  }
}
