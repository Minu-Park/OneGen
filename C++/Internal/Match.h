#pragma once

#include "OneGenTypes.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace onegen {

std::optional<std::string> extract_ipv4(std::string_view s);

// Highest-priority unique match. Throws Error if ambiguous or none.
const DeviceInfo& match_device(const std::vector<DeviceInfo>& devices,
                               std::string_view query);

}  // namespace onegen
