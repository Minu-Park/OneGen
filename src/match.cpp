#include "match.hpp"

#include "onegen/error.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <vector>

namespace onegen {
namespace {

std::string lower(std::string_view s) {
  std::string o(s);
  for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return o;
}

bool eq(std::string_view a, std::string_view b) { return lower(a) == lower(b); }

bool nonempty_eq(std::string_view field, std::string_view q) {
  return !field.empty() && eq(field, q);
}

}  // namespace

std::optional<std::string> extract_ipv4(std::string_view s) {
  static const std::regex re(R"((\d{1,3}(?:\.\d{1,3}){3}))");
  std::string t(s);
  for (std::sregex_iterator it(t.begin(), t.end(), re), end; it != end; ++it) {
    const auto& m = *it;
    const auto pos = static_cast<std::size_t>(m.position());
    const auto tail = pos + static_cast<std::size_t>(m.length());
    if ((pos && std::isdigit(static_cast<unsigned char>(t[pos - 1]))) ||
        (tail < t.size() && std::isdigit(static_cast<unsigned char>(t[tail]))))
      continue;
    const auto ip = m.str();
    std::istringstream parts(ip);
    std::string part;
    bool valid = true;
    while (std::getline(parts, part, '.'))
      if (std::stoi(part) > 255) valid = false;
    if (valid) return ip;
  }
  return std::nullopt;
}

const DeviceInfo& match_device(const std::vector<DeviceInfo>& devices,
                               std::string_view query) {
  if (query.empty()) throw Error("open(): empty query");

  auto collect = [&](auto pred) {
    std::vector<const DeviceInfo*> hit;
    for (const auto& d : devices)
      if (pred(d)) hit.push_back(&d);
    return hit;
  };

  const auto qip = extract_ipv4(query);

  struct Tier {
    const char* name;
    std::vector<const DeviceInfo*> hits;
  };

  std::vector<Tier> tiers = {
      {"id", collect([&](const DeviceInfo& d) { return nonempty_eq(d.id, query); })},
      {"user_defined_name",
       collect([&](const DeviceInfo& d) { return nonempty_eq(d.user_defined_name, query); })},
      {"ip", collect([&](const DeviceInfo& d) {
         if (qip && !d.ip.empty() && d.ip == *qip) return true;
         if (qip && extract_ipv4(d.id) == qip) return true;
         if (qip && extract_ipv4(d.display_name) == qip) return true;
         if (qip && extract_ipv4(d.user_defined_name) == qip) return true;
         return nonempty_eq(d.ip, query);
       })},
      {"serial", collect([&](const DeviceInfo& d) { return nonempty_eq(d.serial, query); })},
      {"display_name",
       collect([&](const DeviceInfo& d) { return nonempty_eq(d.display_name, query); })},
      {"model", collect([&](const DeviceInfo& d) { return nonempty_eq(d.model, query); })},
  };

  for (const auto& t : tiers) {
    if (t.hits.size() == 1) return *t.hits.front();
    if (t.hits.size() > 1) {
      std::ostringstream o;
      o << "open(\"" << query << "\"): " << t.hits.size() << " cameras match " << t.name;
      for (auto* d : t.hits) o << "\n  " << d->id;
      throw Error(o.str());
    }
  }

  std::ostringstream o;
  o << "open(): no camera matches \"" << query << "\"";
  if (devices.empty()) {
    o << " (none enumerated)";
  } else {
    o << "\nfound " << devices.size() << ":";
    for (const auto& d : devices) {
      o << "\n  id=" << d.id;
      if (!d.ip.empty()) o << " ip=" << d.ip;
      if (!d.serial.empty()) o << " serial=" << d.serial;
      if (!d.display_name.empty()) o << " name=" << d.display_name;
    }
  }
  throw Error(o.str());
}

}  // namespace onegen
