#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace onegen {

enum class NodeKind {
  Category,
  Integer,
  Float,
  Boolean,
  Enumeration,
  Command,
  String,
  Other
};

class NodeMap;

struct NodeMapAccess;

class Node {
public:
  std::string name() const;
  std::string display_name() const;
  NodeKind kind() const;
  bool is_readable() const;
  bool is_writable() const;
  bool is_visible() const;

  std::vector<Node> children() const;

  std::int64_t get_int() const;
  void set_int(std::int64_t v);
  std::int64_t int_min() const;
  std::int64_t int_max() const;
  std::int64_t int_inc() const;

  double get_float() const;
  void set_float(double v);
  double float_min() const;
  double float_max() const;

  bool get_bool() const;
  void set_bool(bool v);

  std::string get_string() const;
  void set_string(std::string_view v);

  std::string get_enum() const;
  void set_enum(std::string_view entry);
  std::vector<std::string> enum_entries() const;

  void execute();

  std::string value_text() const;
  void set_value_text(std::string_view v);

  void set(std::int64_t v);
  void set(int v) { set(static_cast<std::int64_t>(v)); }
  void set(double v);
  void set(bool v);
  void set(std::string_view v);
  void set(const char* v) { set(std::string_view(v ? v : "")); }

  std::string get() const { return value_text(); }

  bool valid() const { return inode_ != nullptr; }

private:
  friend class NodeMap;
  NodeMap* map_ = nullptr;
  void* inode_ = nullptr;
};

class NodeMap {
public:
  NodeMap();
  ~NodeMap();
  NodeMap(const NodeMap&) = delete;
  NodeMap& operator=(const NodeMap&) = delete;

  Node root() const;
  Node find(std::string_view name) const;
  bool empty() const;

  using InvalidateFn = std::function<void(std::string_view name)>;
  void set_invalidate_callback(InvalidateFn cb);

  void* raw_nodemap() const { return nodemap_; }

private:
  friend class Camera;
  friend class Node;
  void attach(void* nodemap, void* port);
  void detach();
  void lock() const;
  void unlock() const;

  friend struct NodeMapAccess;
  struct Impl;
  Impl* impl_ = nullptr;
  void* nodemap_ = nullptr;
};

}  // namespace onegen
