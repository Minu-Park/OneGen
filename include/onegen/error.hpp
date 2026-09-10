#pragma once

#include <stdexcept>
#include <string>

namespace onegen {

class Error : public std::runtime_error {
public:
  explicit Error(std::string m) : std::runtime_error(std::move(m)) {}
};

}  // namespace onegen
