#include "Internal/NodeMap.h"
#include "Internal/XmlHttp.h"
#include <fstream>
#include <filesystem>

#include "OneGenError.h"
#include "Internal/StageLog.h"

#include <GenApi/GenApi.h>
#include <GenApi/NodeCallback.h>
#include <GenApi/Persistence.h>
#include <GenApi/EventAdapterGeneric.h>

#include <cctype>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <charconv>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace onegen {
namespace {

class CPort final : public GenApi::IPort {
public:
  Gentl* g = nullptr;
  GenTL::PORT_HANDLE port = nullptr;
  bool open = true;
  std::atomic<std::uint64_t> read_sequence{0};
  std::atomic<std::int64_t> last_success_ns{0};

  void Read(void* buffer, int64_t addr, int64_t length) override {
    if (addr < 0 || length < 0) throw GENERIC_EXCEPTION("IPort::Read: invalid range");
    if (!open || !g || !port) throw GENERIC_EXCEPTION("IPort::Read: closed");
    const auto requested = static_cast<size_t>(length);
    auto size = requested;
    const auto sequence = read_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    // GenTL treats piSize as in/out: a producer updates it with the number of
    // bytes it actually transferred. The Diaphus/Heliotis producer legitimately
    // returns fewer bytes than requested for variable-length register regions,
    // so a short read on GC_ERR_SUCCESS is not an error. Only a producer failure
    // code or a zero-byte transfer indicates a genuine read failure. Any bytes
    // the producer did not fill are zeroed so GenApi parses a defined buffer.
    const auto started = std::chrono::steady_clock::now();
    const auto rc = g->GCReadPort(port, static_cast<uint64_t>(addr), buffer, &size);
    if (rc != GenTL::GC_ERR_SUCCESS || size == 0) {
      const auto started_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
          started.time_since_epoch()).count();
      const auto last_ns = last_success_ns.load(std::memory_order_relaxed);
      const auto since_last_success_ms = last_ns == 0 ? -1
          : started_ns >= last_ns ? (started_ns - last_ns) / 1000000 : 0;
      const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - started).count();
      char producer_error[1024]{};
      auto producer_error_size = sizeof(producer_error) - 1;
      GenTL::GC_ERROR producer_error_code = static_cast<GenTL::GC_ERROR>(GenTL::GC_ERR_ERROR);
      GenTL::GC_ERROR last_error_rc = static_cast<GenTL::GC_ERROR>(GenTL::GC_ERR_ERROR);
      if (rc != GenTL::GC_ERR_SUCCESS && g->GCGetLastError)
        last_error_rc = g->GCGetLastError(
            &producer_error_code, producer_error, &producer_error_size);
      stage_log_warning("worker CPort::Read failed rc=%d port=%p addr=%lld requested=%lld returned=%zu"
                " elapsed_ms=%lld read_seq=%llu since_last_success_ms=%lld"
                " last_error_rc=%d producer_error_code=%d producer_error=\"%s\""
                " (genuine producer read failure or empty transfer)",
                static_cast<int>(rc), static_cast<void*>(port),
                static_cast<long long>(addr), static_cast<long long>(length), size,
                static_cast<long long>(elapsed_ms), static_cast<unsigned long long>(sequence),
                static_cast<long long>(since_last_success_ms), static_cast<int>(last_error_rc),
                static_cast<int>(producer_error_code), producer_error);
      throw GENERIC_EXCEPTION("IPort::Read failed");
    }
    if (size < requested)
      std::memset(static_cast<unsigned char*>(buffer) + size, 0, requested - size);
    last_success_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count(),
        std::memory_order_relaxed);
  }
  void Write(const void* buffer, int64_t addr, int64_t length) override {
    if (addr < 0 || length < 0) throw GENERIC_EXCEPTION("IPort::Write: invalid range");
    if (!open || !g || !port) throw GENERIC_EXCEPTION("IPort::Write: closed");
    auto size = static_cast<size_t>(length);
    if (g->GCWritePort(port, static_cast<uint64_t>(addr), buffer, &size) != GenTL::GC_ERR_SUCCESS
        || size != static_cast<size_t>(length))
      throw GENERIC_EXCEPTION("IPort::Write failed");
  }
  GenApi::EAccessMode GetAccessMode() const override {
    return open ? GenApi::RW : GenApi::NA;
  }
};

std::string to_lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

GenApi::INode* inode(void* p) { return static_cast<GenApi::INode*>(p); }

template <class F>
auto genapi(const char* where, F&& f) -> decltype(f()) {
  try {
    return f();
  } catch (const GENICAM_NAMESPACE::GenericException& e) {
    throw Error(std::string(where) + ": " + e.what());
  }
}

}  // namespace

struct NodeMap::Impl {
  std::recursive_mutex mu;
  std::unique_ptr<GenApi::CNodeMapRef> nm;
  std::unique_ptr<CPort> port;
  std::vector<GenApi::CallbackHandleType> cbs;
  InvalidateFn on_invalidate;
  std::unique_ptr<GenApi::CEventAdapterGeneric> event_adapter;
};

NodeMap::NodeMap() : impl_(new Impl) {}
NodeMap::~NodeMap() {
  detach();
  delete impl_;
}

void NodeMap::lock() const { impl_->mu.lock(); }
void NodeMap::unlock() const { impl_->mu.unlock(); }

void NodeMap::detach() {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  if (impl_->nm && impl_->nm->_Ptr) {
    for (auto h : impl_->cbs) {
      try {
        GenApi::Deregister(h);
      } catch (...) {
      }
    }
  }
  impl_->cbs.clear();
  impl_->event_adapter.reset();
  if (impl_->port) impl_->port->open = false;
  impl_->nm.reset();
  impl_->port.reset();
  nodemap_ = nullptr;
}

void NodeMap::set_invalidate_callback(InvalidateFn cb) {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  impl_->on_invalidate = std::move(cb);
}

bool NodeMap::empty() const {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  return !impl_->nm || !impl_->nm->_Ptr;
}

Node NodeMap::root() const { return find("Root"); }

void NodeMap::poll(std::int64_t elapsed_ms) {
  if (elapsed_ms < 0) throw Error("poll interval cannot be negative");
  genapi("poll", [&] {
    std::lock_guard<std::recursive_mutex> lock(impl_->mu);
    if (!empty()) impl_->nm->_Ptr->Poll(elapsed_ms);
  });
}

void NodeMap::invalidate(std::string_view name) {
  genapi("invalidate", [&] {
    std::lock_guard<std::recursive_mutex> lock(impl_->mu);
    auto node = find(name);
    if (!node.valid()) throw Error("event references an unknown feature");
    inode(node.inode_)->InvalidateNode();
  });
}

void NodeMap::deliver_event(const std::vector<std::uint8_t>& bytes, std::uint64_t event_id) {
  if (bytes.size() > 1024U * 1024U) throw Error("event payload exceeds 1 MiB");
  genapi("event", [&] {
    std::lock_guard<std::recursive_mutex> lock(impl_->mu);
    if (empty()) throw Error("event node map is unavailable");
    if (!impl_->event_adapter)
      impl_->event_adapter = std::make_unique<GenApi::CEventAdapterGeneric>(impl_->nm->_Ptr);
    impl_->event_adapter->DeliverMessage(bytes.data(), static_cast<std::uint32_t>(bytes.size()), event_id);
  });
}

void NodeMap::deliver_event(const std::vector<std::uint8_t>& bytes, std::string_view event_id) {
  if (bytes.size() > 1024U * 1024U || event_id.size() > 65536)
    throw Error("event payload or identifier exceeds supported bounds");
  genapi("event", [&] {
    std::lock_guard<std::recursive_mutex> lock(impl_->mu);
    if (empty()) throw Error("event node map is unavailable");
    if (!impl_->event_adapter)
      impl_->event_adapter = std::make_unique<GenApi::CEventAdapterGeneric>(impl_->nm->_Ptr);
    impl_->event_adapter->DeliverMessage(bytes.data(), static_cast<std::uint32_t>(bytes.size()),
        GENICAM_NAMESPACE::gcstring(std::string(event_id).c_str()));
  });
}

std::string NodeMap::save_features() const {
  return genapi("save features", [&] {
    std::lock_guard<std::recursive_mutex> lock(impl_->mu);
    if (empty()) throw Error("feature node map is unavailable");
    GenApi::CFeatureBag bag;
    bag.StoreToBag(impl_->nm->_Ptr, 100000);
    std::ostringstream output;
    output << bag;
    auto contents = output.str();
    if (contents.size() > maximum_xml_size) throw Error("feature bag exceeds 32 MiB");
    return contents;
  });
}

void NodeMap::load_features(std::string_view contents) {
  if (contents.size() > maximum_xml_size) throw Error("feature bag exceeds 32 MiB");
  genapi("load features", [&] {
    std::lock_guard<std::recursive_mutex> lock(impl_->mu);
    if (empty()) throw Error("feature node map is unavailable");
    GenApi::CFeatureBag bag;
    std::istringstream input{std::string(contents)};
    input >> bag;
    if (input.fail()) throw Error("invalid GenApi feature bag");
    if (!bag.LoadFromBag(impl_->nm->_Ptr, true)) throw Error("feature bag restore or verification failed");
  });
}

Node NodeMap::find(std::string_view name) const {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  Node n;
  n.map_ = const_cast<NodeMap*>(this);
  if (!impl_->nm || !impl_->nm->_Ptr) return n;
  n.inode_ = impl_->nm->_Ptr->GetNode(GENICAM_NAMESPACE::gcstring(std::string(name).c_str()));
  return n;
}

void NodeMap::attach(void*, void*) {}

struct NodeMapAccess {
  static void load(NodeMap& map, Gentl& g, GenTL::PORT_HANDLE port);
  static void load_string(NodeMap& map, const char* xml);
};

void load_remote_nodemap(NodeMap& map, Gentl& g, GenTL::PORT_HANDLE port,
                         std::string_view module,
                         std::function<void(std::string_view)> diagnostic) {
  try {
    NodeMapAccess::load(map, g, port);
  } catch (const std::exception& error) {
    if (diagnostic) {
      const auto message = std::string(module) + " XML/node map loading failed: " + error.what();
      try { diagnostic(message); } catch (...) { /* Diagnostics must not replace the load failure. */ }
    }
    throw;
  } catch (...) {
    if (diagnostic) {
      try { diagnostic(std::string(module) + " XML/node map loading failed: unknown exception"); }
      catch (...) { /* Diagnostics must not replace the load failure. */ }
    }
    throw;
  }
}

void load_string_nodemap(NodeMap& map, const char* xml) {
  NodeMapAccess::load_string(map, xml);
}

void NodeMapAccess::load(NodeMap& map, Gentl& g, GenTL::PORT_HANDLE port) {
  map.detach();
  std::lock_guard<std::recursive_mutex> lk(map.impl_->mu);
  try {

  uint32_t nurl = 0;
  g.check(g.GCGetNumPortURLs(port, &nurl), "GCGetNumPortURLs");
  if (nurl == 0) throw Error("device port has no XML URL");
  if (nurl > 16) throw Error("device port exposes more than 16 XML URLs");

  std::unique_ptr<GenApi::CNodeMapRef> nm;
  std::string failures;
  for (std::uint32_t index = 0; index < nurl; ++index) {
    try {
      GenTL::INFO_DATATYPE type = GenTL::INFO_DATATYPE_UNKNOWN;
      std::size_t size = 0;
      const auto query = g.GCGetPortURLInfo(port, index, GenTL::URL_INFO_URL, &type, nullptr, &size);
      if (query != GenTL::GC_ERR_SUCCESS && query != GenTL::GC_ERR_BUFFER_TOO_SMALL)
        g.check(query, "GCGetPortURLInfo(size)");
      if (!size || size > 65536) throw Error("XML URL length is invalid");
      std::vector<char> url_bytes(size + 1, 0);
      const auto capacity = size;
      g.check(g.GCGetPortURLInfo(port, index, GenTL::URL_INFO_URL, &type, url_bytes.data(), &size), "GCGetPortURLInfo");
      if (size > capacity || type != GenTL::INFO_DATATYPE_STRING) throw Error("XML URL response is invalid");
      const std::string url(url_bytes.data());
      const auto scheme = to_lower(url);
      auto candidate = std::make_unique<GenApi::CNodeMapRef>();
      std::vector<char> xml;
      bool zip = false;
      if (scheme.rfind("local:", 0) == 0) {
        const auto first = url.find(';');
        const auto second = first == std::string::npos ? first : url.find(';', first + 1);
        if (second == std::string::npos) throw Error("Malformed local XML URL");
        const auto parse_hex = [](std::string_view field) {
          const auto begin = field.find_first_not_of(" \t\r\n");
          if (begin == std::string_view::npos) throw Error("Invalid local XML address or length");
          field = field.substr(begin, field.find_last_not_of(" \t\r\n") - begin + 1);
          if (field.starts_with("0x") || field.starts_with("0X")) field.remove_prefix(2);
          std::uint64_t value = 0;
          const auto parsed = std::from_chars(field.data(), field.data() + field.size(), value, 16);
          if (parsed.ec != std::errc{} || parsed.ptr != field.data() + field.size())
            throw Error("Invalid local XML address or length");
          return value;
        };
        const auto address = parse_hex(std::string_view(url).substr(first + 1, second - first - 1));
        const auto third = url.find_first_of(";?", second + 1);
        const auto length = parse_hex(std::string_view(url).substr(second + 1, third - second - 1));
        if (!length || length > maximum_xml_size || address > UINT64_MAX - length)
          throw Error("Local XML length or address is outside supported bounds");
        xml.resize(static_cast<std::size_t>(length));
        size = xml.size();
        g.check(g.GCReadPort(port, address, xml.data(), &size), "GCReadPort(xml)");
        if (size != xml.size()) throw Error("Short local XML read");
        zip = to_lower(url.substr(0, first)).ends_with(".zip");
      } else if (scheme.rfind("file:", 0) == 0) {
        std::string path = url.substr(5);
#ifdef _WIN32
        if (path.starts_with("///")) path.erase(0, 3);
#else
        if (path.starts_with("///")) path.erase(0, 2);
        else if (path.starts_with("//localhost/")) path.erase(0, 11);
        else if (path.starts_with("//")) throw Error("Remote file URL authorities are unsupported on this platform");
#endif
        std::string decoded;
        for (std::size_t position = 0; position < path.size(); ++position) {
          if (path[position] != '%') { decoded.push_back(path[position]); continue; }
          unsigned int byte = 0;
          if (position + 2 >= path.size()) throw Error("Malformed escaped file XML URL");
          const auto parsed = std::from_chars(path.data() + position + 1, path.data() + position + 3, byte, 16);
          if (parsed.ec != std::errc{} || parsed.ptr != path.data() + position + 3 || byte == 0)
            throw Error("Malformed escaped file XML URL");
          decoded.push_back(static_cast<char>(byte));
          position += 2;
        }
        std::ifstream input(std::filesystem::u8path(decoded), std::ios::binary | std::ios::ate);
        if (!input) throw Error("Could not open XML file");
        const auto length = input.tellg();
        if (length <= 0 || length > static_cast<std::streamoff>(maximum_xml_size))
          throw Error("XML file length is outside supported bounds");
        xml.resize(static_cast<std::size_t>(length));
        input.seekg(0);
        if (!input.read(xml.data(), static_cast<std::streamsize>(xml.size()))) throw Error("Short XML file read");
        zip = to_lower(path).ends_with(".zip");
      } else if (scheme.starts_with("http:") || scheme.starts_with("https:")) {
        xml = fetch_xml_http(url);
        zip = xml.size() >= 4 && xml[0] == 'P' && xml[1] == 'K' && xml[2] == 3 && xml[3] == 4;
      } else {
        throw Error("Unsupported XML URL scheme");
      }
      if (zip) candidate->_LoadXMLFromZIPData(xml.data(), xml.size());
      else {
        xml.push_back('\0');
        candidate->_LoadXMLFromString(GENICAM_NAMESPACE::gcstring(xml.data()));
      }
      // A valid XML with a failing port connection also permits the next URL.
      std::size_t name_size = 0;
      const auto name_query = g.GCGetPortInfo(port, GenTL::PORT_INFO_PORTNAME, &type, nullptr, &name_size);
      if (name_query != GenTL::GC_ERR_SUCCESS && name_query != GenTL::GC_ERR_BUFFER_TOO_SMALL)
        g.check(name_query, "GCGetPortInfo(size)");
      if (!name_size || name_size > 65536) throw Error("Port name length is invalid");
      std::vector<char> name(name_size + 1, 0);
      const auto name_capacity = name_size;
      g.check(g.GCGetPortInfo(port, GenTL::PORT_INFO_PORTNAME, &type, name.data(), &name_size), "GCGetPortInfo");
      if (name_size > name_capacity || type != GenTL::INFO_DATATYPE_STRING) throw Error("Port name response is invalid");
      auto candidate_port = std::make_unique<CPort>();
      candidate_port->g = &g;
      candidate_port->port = port;
      if (!candidate->_Connect(candidate_port.get(), name.data())) throw Error("NodeMap port connection failed");
      map.impl_->port = std::move(candidate_port);
      nm = std::move(candidate);
      break;
    } catch (const std::exception& error) {
      failures += "URL[" + std::to_string(index) + "]: " + error.what() + "\n";
    } catch (const GENICAM_NAMESPACE::GenericException& error) {
      failures += "URL[" + std::to_string(index) + "]: " + error.what() + "\n";
    }
  }
  if (!nm) throw Error("All XML URLs failed:\n" + failures);

  if (nm->_Ptr) {
    GenApi::NodeList_t nodes;
    nm->_Ptr->GetNodes(nodes);
    for (size_t i = 0; i < nodes.size(); ++i) {
      GenApi::INode* node = nodes[i];
      if (!node) continue;
      // GenApi dispatches this callback from inside its own machinery (and the
      // producer can dispatch it on its event thread). That dispatch frame is a
      // de-facto noexcept/C boundary: any exception escaping here, including a
      // GenApi GenericException from GetName() or a std::runtime_error from a
      // failed host-pipe write inside the invalidate sink, crosses that frame
      // with no handler and calls std::terminate. A node-invalidate notification
      // must never propagate an exception back into GenApi, so swallow every failure.
      std::function<void(GenApi::INode*)> fn = [&map](GenApi::INode* n) noexcept {
        try {
          if (!n) return;
          const std::string name = n->GetName().c_str();
          NodeMap::InvalidateFn cb;
          {
            std::lock_guard<std::recursive_mutex> lk(map.impl_->mu);
            cb = map.impl_->on_invalidate;
          }
          if (cb) cb(name);
        } catch (...) {
          // A dropped invalidation on a dead channel is acceptable; a terminate is not.
        }
      };
      map.impl_->cbs.push_back(GenApi::Register(node, fn, GenApi::cbPostOutsideLock));
    }
  }

  map.impl_->nm = std::move(nm);

  map.nodemap_ = map.impl_->nm.get();
  } catch (const GENICAM_NAMESPACE::GenericException& e) {
    throw Error(std::string("nodemap: ") + e.what());
  }
}

void NodeMapAccess::load_string(NodeMap& map, const char* xml) {
  map.detach();
  std::lock_guard<std::recursive_mutex> lk(map.impl_->mu);
  genapi("load xml", [&] {
    auto nm = std::make_unique<GenApi::CNodeMapRef>();
    nm->_LoadXMLFromString(GENICAM_NAMESPACE::gcstring(xml));
    map.impl_->nm = std::move(nm);
    map.nodemap_ = map.impl_->nm.get();
  });
}

std::string Node::name() const {
  if (!inode_) return {};
  std::lock_guard<std::recursive_mutex> lk(map_->impl_->mu);
  return inode(inode_)->GetName().c_str();
}

std::string Node::display_name() const {
  if (!inode_) return {};
  std::lock_guard<std::recursive_mutex> lk(map_->impl_->mu);
  return inode(inode_)->GetDisplayName().c_str();
}

std::string Node::description() const {
  if (!inode_) return {};
  std::lock_guard<std::recursive_mutex> lk(map_->impl_->mu);
  return inode(inode_)->GetDescription().c_str();
}

std::string Node::tooltip() const {
  if (!inode_) return {};
  std::lock_guard<std::recursive_mutex> lk(map_->impl_->mu);
  return inode(inode_)->GetToolTip().c_str();
}

NodeKind Node::kind() const {
  if (!inode_) return NodeKind::Other;
  std::lock_guard<std::recursive_mutex> lk(map_->impl_->mu);
  switch (inode(inode_)->GetPrincipalInterfaceType()) {
    case GenApi::intfICategory:
      return NodeKind::Category;
    case GenApi::intfIInteger:
      return NodeKind::Integer;
    case GenApi::intfIFloat:
      return NodeKind::Float;
    case GenApi::intfIBoolean:
      return NodeKind::Boolean;
    case GenApi::intfIEnumeration:
      return NodeKind::Enumeration;
    case GenApi::intfICommand:
      return NodeKind::Command;
    case GenApi::intfIString:
      return NodeKind::String;
    case GenApi::intfIRegister:
      return NodeKind::Register;
    default:
      return NodeKind::Other;
  }
}

bool Node::is_readable() const {
  if (!inode_) return false;
  std::lock_guard<std::recursive_mutex> lk(map_->impl_->mu);
  return GenApi::IsReadable(inode(inode_));
}
bool Node::is_writable() const {
  if (!inode_) return false;
  std::lock_guard<std::recursive_mutex> lk(map_->impl_->mu);
  return GenApi::IsWritable(inode(inode_));
}
bool Node::is_visible() const {
  if (!inode_) return false;
  std::lock_guard<std::recursive_mutex> lk(map_->impl_->mu);
  return inode(inode_)->GetVisibility() != GenApi::Invisible;
}

std::vector<Node> Node::children() const {
  std::vector<Node> out;
  if (!inode_ || kind() != NodeKind::Category) return out;
  std::lock_guard<std::recursive_mutex> lk(map_->impl_->mu);
  GenApi::CCategoryPtr cat(inode(inode_));
  if (!cat.IsValid()) return out;
  GenApi::FeatureList_t feats;
  cat->GetFeatures(feats);
  for (size_t i = 0; i < feats.size(); ++i) {
    GenApi::IValue* f = feats[i];
    if (!f) continue;
    Node c;
    c.map_ = map_;
    c.inode_ = f->GetNode();
    if (c.valid() && c.is_visible()) out.push_back(c);
  }
  return out;
}

std::vector<Node> Node::selecting_features() const {
  std::vector<Node> out;
  if (!inode_) return out;
  std::lock_guard<std::recursive_mutex> lk(map_->impl_->mu);
  GenApi::CSelectorPtr selector(inode(inode_));
  if (!selector.IsValid()) return out;
  GenApi::FeatureList_t features;
  selector->GetSelectingFeatures(features);
  for (auto* feature : features) {
    if (!feature) continue;
    Node selected; selected.map_ = map_; selected.inode_ = feature->GetNode();
    if (selected.valid()) out.push_back(selected);
  }
  return out;
}

std::int64_t Node::get_int() const {
  return genapi("get_int", [&] {
    std::lock_guard<std::recursive_mutex> lk(map_->impl_->mu);
    return GenApi::CIntegerPtr(inode(inode_))->GetValue();
  });
}
void Node::set_int(std::int64_t v) {
  genapi("set_int", [&] {
    std::lock_guard<std::recursive_mutex> lk(map_->impl_->mu);
    GenApi::CIntegerPtr p(inode(inode_));
    if (!GenApi::IsWritable(p)) throw Error("integer not writable");
    p->SetValue(v);
  });
}
std::int64_t Node::int_min() const {
  return genapi("int_min", [&] {
    std::lock_guard<std::recursive_mutex> lk(map_->impl_->mu);
    return GenApi::CIntegerPtr(inode(inode_))->GetMin();
  });
}
std::int64_t Node::int_max() const {
  return genapi("int_max", [&] {
    std::lock_guard<std::recursive_mutex> lk(map_->impl_->mu);
    return GenApi::CIntegerPtr(inode(inode_))->GetMax();
  });
}
std::int64_t Node::int_inc() const {
  return genapi("int_inc", [&] {
    std::lock_guard<std::recursive_mutex> lk(map_->impl_->mu);
    return GenApi::CIntegerPtr(inode(inode_))->GetInc();
  });
}

double Node::get_float() const {
  return genapi("get_float", [&] {
    std::lock_guard<std::recursive_mutex> lk(map_->impl_->mu);
    return GenApi::CFloatPtr(inode(inode_))->GetValue();
  });
}
void Node::set_float(double v) {
  genapi("set_float", [&] {
    std::lock_guard<std::recursive_mutex> lk(map_->impl_->mu);
    GenApi::CFloatPtr p(inode(inode_));
    if (!GenApi::IsWritable(p)) throw Error("float not writable");
    p->SetValue(v);
  });
}
double Node::float_min() const {
  return genapi("float_min", [&] {
    std::lock_guard<std::recursive_mutex> lk(map_->impl_->mu);
    return GenApi::CFloatPtr(inode(inode_))->GetMin();
  });
}
double Node::float_max() const {
  return genapi("float_max", [&] {
    std::lock_guard<std::recursive_mutex> lk(map_->impl_->mu);
    return GenApi::CFloatPtr(inode(inode_))->GetMax();
  });
}

double Node::float_inc() const {
  return genapi("float_inc", [&] {
    std::lock_guard<std::recursive_mutex> lock(map_->impl_->mu);
    GenApi::CFloatPtr node(inode(inode_));
    return node->HasInc() ? node->GetInc() : 0.0;
  });
}

std::string Node::unit() const {
  return genapi("unit", [&]() -> std::string {
    std::lock_guard<std::recursive_mutex> lock(map_->impl_->mu);
    if (kind() == NodeKind::Float) return GenApi::CFloatPtr(inode(inode_))->GetUnit().c_str();
    if (kind() == NodeKind::Integer) return GenApi::CIntegerPtr(inode(inode_))->GetUnit().c_str();
    return {};
  });
}

std::string Node::representation() const {
  return genapi("representation", [&]() -> std::string {
    std::lock_guard<std::recursive_mutex> lock(map_->impl_->mu);
    const auto value = kind() == NodeKind::Float
        ? GenApi::CFloatPtr(inode(inode_))->GetRepresentation()
        : GenApi::CIntegerPtr(inode(inode_))->GetRepresentation();
    switch (value) {
      case GenApi::HexNumber: return "Hexadecimal";
      case GenApi::IPV4Address: return "IPv4";
      case GenApi::MACAddress: return "MAC address";
      case GenApi::Logarithmic: return "Logarithmic";
      case GenApi::Linear: return "Linear";
      default: return "Decimal";
    }
  });
}

bool Node::get_bool() const {
  return genapi("get_bool", [&] {
    std::lock_guard<std::recursive_mutex> lk(map_->impl_->mu);
    return GenApi::CBooleanPtr(inode(inode_))->GetValue();
  });
}
void Node::set_bool(bool v) {
  genapi("set_bool", [&] {
    std::lock_guard<std::recursive_mutex> lk(map_->impl_->mu);
    GenApi::CBooleanPtr p(inode(inode_));
    if (!GenApi::IsWritable(p)) throw Error("bool not writable");
    p->SetValue(v);
  });
}

std::string Node::get_string() const {
  return genapi("get_string", [&] {
    std::lock_guard<std::recursive_mutex> lk(map_->impl_->mu);
    return std::string(GenApi::CStringPtr(inode(inode_))->GetValue().c_str());
  });
}
void Node::set_string(std::string_view v) {
  genapi("set_string", [&] {
    std::lock_guard<std::recursive_mutex> lk(map_->impl_->mu);
    GenApi::CStringPtr p(inode(inode_));
    if (!GenApi::IsWritable(p)) throw Error("string not writable");
    p->SetValue(std::string(v).c_str());
  });
}

std::string Node::get_enum() const {
  return genapi("get_enum", [&] {
    std::lock_guard<std::recursive_mutex> lk(map_->impl_->mu);
    return std::string(
        GenApi::CEnumerationPtr(inode(inode_))->GetCurrentEntry()->GetSymbolic().c_str());
  });
}
std::int64_t Node::get_enum_value() const {
  return genapi("get_enum_value", [&] {
    std::lock_guard<std::recursive_mutex> lk(map_->impl_->mu);
    return GenApi::CEnumerationPtr(inode(inode_))->GetCurrentEntry()->GetValue();
  });
}
void Node::set_enum(std::string_view entry) {
  genapi("set_enum", [&] {
    std::lock_guard<std::recursive_mutex> lk(map_->impl_->mu);
    GenApi::CEnumerationPtr p(inode(inode_));
    if (!GenApi::IsWritable(p)) throw Error("enum not writable");
    p->FromString(std::string(entry).c_str());
  });
}
std::vector<std::string> Node::enum_entries() const {
  return genapi("enum_entries", [&] {
    std::lock_guard<std::recursive_mutex> lk(map_->impl_->mu);
    GenApi::CEnumerationPtr p(inode(inode_));
    GenApi::NodeList_t entries;
    p->GetEntries(entries);
    std::vector<std::string> out;
    for (auto* e : entries) {
      GenApi::CEnumEntryPtr ee(e);
      if (ee.IsValid() && GenApi::IsAvailable(ee))
        out.emplace_back(ee->GetSymbolic().c_str());
    }
    return out;
  });
}

void Node::execute() {
  genapi("execute", [&] {
    std::lock_guard<std::recursive_mutex> lk(map_->impl_->mu);
    GenApi::CCommandPtr p(inode(inode_));
    if (!p.IsValid()) throw Error("not a command");
    p->Execute();
  });
}

std::uint64_t Node::register_length() const {
  if (kind() != NodeKind::Register) throw Error("not a raw register node");
  return genapi("register length", [&] {
    std::lock_guard<std::recursive_mutex> lock(map_->impl_->mu);
    return static_cast<std::uint64_t>(GenApi::CRegisterPtr(inode(inode_))->GetLength());
  });
}

std::vector<std::uint8_t> Node::read_register() const {
  return genapi("read register", [&] {
    std::lock_guard<std::recursive_mutex> lock(map_->impl_->mu);
    const auto length = register_length();
    if (!is_readable() || !length || length > 65536) throw Error("register is unreadable or exceeds 64 KiB");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    GenApi::CRegisterPtr(inode(inode_))->Get(bytes.data(), length);
    return bytes;
  });
}

void Node::write_register(const std::vector<std::uint8_t>& bytes) {
  genapi("write register", [&] {
    std::lock_guard<std::recursive_mutex> lock(map_->impl_->mu);
    if (!is_writable() || bytes.empty() || bytes.size() > 65536 || bytes.size() != register_length())
      throw Error("register is unwritable or its byte length does not match");
    GenApi::CRegisterPtr(inode(inode_))->Set(bytes.data(), bytes.size());
  });
}

std::string Node::value_text() const {
  if (!valid() || !is_readable()) return {};
  return genapi("get", [&] {
    std::lock_guard<std::recursive_mutex> lk(map_->impl_->mu);
    GenApi::CValuePtr p(inode(inode_));
    if (!p.IsValid()) return std::string{};
    return std::string(p->ToString().c_str());
  });
}

void Node::set(std::string_view v) {
  if (!valid()) throw Error("feature: invalid node");
  const auto k = kind();
  if (k == NodeKind::Category || k == NodeKind::Command)
    throw Error(std::string("feature: cannot set ") + name() +
                (k == NodeKind::Command ? " (use execute())" : ""));
  genapi("set", [&] {
    std::lock_guard<std::recursive_mutex> lk(map_->impl_->mu);
    GenApi::CValuePtr p(inode(inode_));
    if (!p.IsValid()) throw Error("feature: not a value node: " + name());
    if (!GenApi::IsWritable(p)) throw Error("feature: not writable: " + name());
    p->FromString(GENICAM_NAMESPACE::gcstring(std::string(v).c_str()));
  });
}

void Node::set(std::int64_t v) {
  if (!valid()) throw Error("feature: invalid node");
  switch (kind()) {
    case NodeKind::Integer:
      set_int(v);
      break;
    case NodeKind::Float:
      set_float(static_cast<double>(v));
      break;
    case NodeKind::Boolean:
      set_bool(v != 0);
      break;
    case NodeKind::Enumeration:
    case NodeKind::String:
      set(std::string_view(std::to_string(v)));
      break;
    default:
      throw Error("feature: cannot set integer on " + name());
  }
}

void Node::set(double v) {
  if (!valid()) throw Error("feature: invalid node");
  switch (kind()) {
    case NodeKind::Float:
      set_float(v);
      break;
    case NodeKind::Integer:
      set_int(static_cast<std::int64_t>(v));
      break;
    default:
      throw Error("feature: cannot set float on " + name());
  }
}

void Node::set(bool v) {
  if (!valid()) throw Error("feature: invalid node");
  if (kind() != NodeKind::Boolean) throw Error("feature: cannot set bool on " + name());
  set_bool(v);
}

/** @brief Copies scalar metadata and optionally expands category/selector relations once. */
static FeatureNode snapshotNode(const Node& node, bool relations)
{
  FeatureNode out;
  if (!node.valid()) return out;
  out.name = node.name();
  out.display_name = node.display_name();
  out.description = node.description();
  out.tooltip = node.tooltip();
  out.kind = node.kind();
  out.readable = node.is_readable();
  out.writable = node.is_writable();
  out.visible = node.is_visible();
  try {
    switch (out.kind) {
      case NodeKind::Integer:
        if (out.readable) out.int_value = node.get_int();
        out.int_min = node.int_min();
        out.int_max = node.int_max();
        out.int_inc = node.int_inc();
        out.unit = node.unit();
        out.representation = node.representation();
        break;
      case NodeKind::Float:
        if (out.readable) out.float_value = node.get_float();
        out.float_min = node.float_min();
        out.float_max = node.float_max();
        out.float_inc = node.float_inc();
        out.unit = node.unit();
        out.representation = node.representation();
        break;
      case NodeKind::Boolean:
        if (out.readable) out.bool_value = node.get_bool();
        break;
      case NodeKind::Enumeration:
        out.enum_entries = node.enum_entries();
        if (out.readable) out.enum_value = node.get_enum();
        break;
      case NodeKind::String:
        if (out.readable) out.string_value = node.get_string();
        break;
      case NodeKind::Register:
        out.register_length = node.register_length();
        break;
      default:
        if (out.readable) out.string_value = node.value_text();
        break;
    }
  } catch (const std::exception& error) {
    out.observation_error = error.what();
  } catch (...) {
    out.observation_error = "Feature value or constraint observation failed.";
  }
  if (relations) {
    if (out.kind == NodeKind::Category) {
      for (const auto& child : node.children()) out.children.push_back(snapshotNode(child, true));
    }
    for (const auto& selector : node.selecting_features())
      out.selecting_features.push_back(snapshotNode(selector, false));
  }
  return out;
}

FeatureNode snapshot(const Node& node) { return snapshotNode(node, true); }

FeatureNode snapshot_parameter(const Node& node) {
  auto result = snapshotNode(node, false);
  std::map<std::string, FeatureNode> selectors;
  std::set<std::string> active{node.name()};
  std::function<void(const Node&)> collect = [&](const Node& selected) {
    for (const auto& selector : selected.selecting_features()) {
      const auto name = selector.name();
      if (active.count(name)) throw Error("Cyclic selecting-feature context.");
      if (selectors.count(name)) continue;
      if (selectors.size() + active.size() > 128) throw Error("Selecting-feature context exceeds observation limit.");
      active.insert(name); collect(selector); active.erase(name);
      selectors.emplace(name, snapshotNode(selector, false));
    }
  };
  try { collect(node); }
  catch (const std::exception& error) { result.observation_error += std::string("; selector context: ") + error.what(); }
  for (auto& [name, selector] : selectors) result.selecting_features.push_back(std::move(selector));
  return result;
}

bool same_parameter_context(const FeatureNode& current, const FeatureNode& expected, bool compareValue) {
  const auto scalarEqual = [](const FeatureNode& a, const FeatureNode& b) {
    if (a.name != b.name || a.kind != b.kind || a.readable != b.readable
        || !a.observation_error.empty() || !b.observation_error.empty()) return false;
    if (!a.readable) return true;
    switch (a.kind) {
      case NodeKind::Integer: return a.int_value == b.int_value;
      case NodeKind::Float: return a.float_value == b.float_value;
      case NodeKind::Boolean: return a.bool_value == b.bool_value;
      case NodeKind::Enumeration: return a.enum_value == b.enum_value;
      case NodeKind::String: return a.string_value == b.string_value;
      case NodeKind::Command: return true;
      default: return false;
    }
  };
  if (current.name != expected.name || current.kind != expected.kind || current.readable != expected.readable
      || current.writable != expected.writable || !current.observation_error.empty() || !expected.observation_error.empty()) return false;
  if (current.display_name != expected.display_name || current.description != expected.description || current.unit != expected.unit) return false;
  if (current.kind == NodeKind::Integer && (current.int_min != expected.int_min || current.int_max != expected.int_max || current.int_inc != expected.int_inc)) return false;
  if (current.kind == NodeKind::Float && (current.float_min != expected.float_min || current.float_max != expected.float_max || current.float_inc != expected.float_inc)) return false;
  if (current.kind == NodeKind::Enumeration && current.enum_entries != expected.enum_entries) return false;
  if (compareValue && !scalarEqual(current, expected)) return false;
  if (current.selecting_features.size() != expected.selecting_features.size()) return false;
  for (std::size_t i = 0; i < current.selecting_features.size(); ++i)
    if (!current.selecting_features[i].readable || !scalarEqual(current.selecting_features[i], expected.selecting_features[i])) return false;
  return true;
}

FeatureNode Node::parameter_snapshot() const {
  if (!valid()) return {};
  std::lock_guard<std::recursive_mutex> lock(map_->impl_->mu);
  return snapshot_parameter(*this);
}
bool Node::apply_checked(std::string_view operation, std::string_view value,
    const FeatureNode& expected, bool compareValue) {
  if (!valid()) return false;
  std::lock_guard<std::recursive_mutex> lock(map_->impl_->mu);
  const auto current = snapshot_parameter(*this);
  if (!current.writable || !same_parameter_context(current, expected, compareValue)) return false;
  apply_feature(*this, operation, value);
  return true;
}

void apply_feature(Node node, std::string_view op, std::string_view value)
{
  if (!node.valid()) throw Error("feature: invalid node");
  const std::string text(value);
  if (op == "int") {
    std::int64_t number = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
      throw Error("integer value is invalid or outside the signed 64-bit range");
    node.set_int(number);
  } else if (op == "float") {
    double number = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(number))
      throw Error("floating-point value must be finite and completely parsed");
    node.set_float(number);
  } else if (op == "bool") {
    if (text != "0" && text != "1" && text != "true" && text != "false")
      throw Error("invalid boolean value");
    node.set_bool(text == "1" || text == "true");
  }
  else if (op == "string") node.set_string(text);
  else if (op == "enum") node.set_enum(text);
  else if (op == "execute") node.execute();
  else throw Error("feature: unsupported operation " + std::string(op));
}

}  // namespace onegen
