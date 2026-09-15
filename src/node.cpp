#include "nodemap.hpp"

#include "onegen/error.hpp"

#include <GenApi/GenApi.h>
#include <GenApi/NodeCallback.h>

#include <cctype>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
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

  void Read(void* buffer, int64_t addr, int64_t length) override {
    if (!open || !g || !port) throw GENERIC_EXCEPTION("IPort::Read: closed");
    auto size = static_cast<size_t>(length);
    if (g->GCReadPort(port, static_cast<uint64_t>(addr), buffer, &size) != GenTL::GC_ERR_SUCCESS)
      throw GENERIC_EXCEPTION("IPort::Read failed");
  }
  void Write(const void* buffer, int64_t addr, int64_t length) override {
    if (!open || !g || !port) throw GENERIC_EXCEPTION("IPort::Write: closed");
    auto size = static_cast<size_t>(length);
    if (g->GCWritePort(port, static_cast<uint64_t>(addr), buffer, &size) != GenTL::GC_ERR_SUCCESS)
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

void load_remote_nodemap(NodeMap& map, Gentl& g, GenTL::PORT_HANDLE port) {
  NodeMapAccess::load(map, g, port);
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

  char urlbuf[1024] = {};
  size_t urlsize = sizeof(urlbuf);
  GenTL::INFO_DATATYPE type = GenTL::INFO_DATATYPE_UNKNOWN;
  g.check(g.GCGetPortURLInfo(port, 0, GenTL::URL_INFO_URL, &type, urlbuf, &urlsize),
          "GCGetPortURLInfo");
  const std::string url = urlbuf;
  const std::string url_l = to_lower(url);

  auto nm = std::make_unique<GenApi::CNodeMapRef>();
  if (url_l.rfind("local:", 0) == 0) {
    size_t i = 6;
    if (url.compare(i, 3, "///") == 0) i += 3;
    std::string rest = url.substr(i);
    std::string name, saddr, slen;
    {
      std::istringstream in(rest);
      std::getline(in, name, ';');
      std::getline(in, saddr, ';');
      std::getline(in, slen, ';');
    }
    const uint64_t address = std::stoull(saddr, nullptr, 16);
    auto length = static_cast<size_t>(std::stoull(slen, nullptr, 16));
    std::vector<char> buf(length + 1, 0);
    size_t n = length;
    g.check(g.GCReadPort(port, address, buf.data(), &n), "GCReadPort(xml)");
    const bool zip = name.size() >= 4 && to_lower(name.substr(name.size() - 4)) == ".zip";
    if (zip) {
      nm->_LoadXMLFromZIPData(buf.data(), n);
    } else {
      nm->_LoadXMLFromString(GENICAM_NAMESPACE::gcstring(buf.data()));
    }
  } else if (url_l.rfind("file:", 0) == 0) {
    size_t i = 5;
    if (url.compare(i, 3, "///") == 0) i += 3;
    std::string path = url.substr(i);
    if (path.size() >= 4 && to_lower(path.substr(path.size() - 4)) == ".zip")
      nm->_LoadXMLFromZIPFile(path.c_str());
    else
      nm->_LoadXMLFromFile(path.c_str());
  } else {
    throw Error("unsupported port URL: " + url);
  }

  char pname[256] = {};
  size_t pns = sizeof(pname);
  g.check(g.GCGetPortInfo(port, GenTL::PORT_INFO_PORTNAME, &type, pname, &pns), "GCGetPortInfo");

  auto cport = std::make_unique<CPort>();
  cport->g = &g;
  cport->port = port;
  if (!nm->_Connect(cport.get(), pname)) throw Error(std::string("NodeMap connect failed: ") + pname);

  if (nm->_Ptr) {
    GenApi::NodeList_t nodes;
    nm->_Ptr->GetNodes(nodes);
    for (size_t i = 0; i < nodes.size(); ++i) {
      GenApi::INode* node = nodes[i];
      if (!node) continue;
      std::function<void(GenApi::INode*)> fn = [&map](GenApi::INode* n) {
        if (!n) return;
        const std::string name = n->GetName().c_str();
        NodeMap::InvalidateFn cb;
        {
          std::lock_guard<std::recursive_mutex> lk(map.impl_->mu);
          cb = map.impl_->on_invalidate;
        }
        if (cb) cb(name);
      };
      map.impl_->cbs.push_back(GenApi::Register(node, fn, GenApi::cbPostOutsideLock));
    }
  }

  map.impl_->nm = std::move(nm);
  map.impl_->port = std::move(cport);
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

std::string Node::value_text() const {
  if (!valid() || !is_readable()) return {};
  return genapi("get", [&] {
    std::lock_guard<std::recursive_mutex> lk(map_->impl_->mu);
    GenApi::CValuePtr p(inode(inode_));
    if (!p.IsValid()) return std::string{};
    return std::string(p->ToString().c_str());
  });
}

void Node::set_value_text(std::string_view v) { set(v); }

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

}  // namespace onegen
