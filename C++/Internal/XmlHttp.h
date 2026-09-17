#pragma once
#include <string>
#include <vector>

namespace onegen {
/** Retrieves HTTP(S) XML with bounded time and size and normal certificate verification. */
std::vector<char> fetch_xml_http(const std::string& url);
inline constexpr std::size_t maximum_xml_size = 32U * 1024U * 1024U;
}
