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

/** Copied GenApi node record with no vendor pointers. */
struct FeatureNode {
  std::string name;
  std::string display_name;
  std::string description;
  std::string tooltip;
  NodeKind kind = NodeKind::Other;
  bool readable = false;
  bool writable = false;
  bool visible = false;
  std::int64_t int_value = 0;
  std::int64_t int_min = 0;
  std::int64_t int_max = 0;
  std::int64_t int_inc = 1;
  double float_value = 0;
  double float_min = 0;
  double float_max = 0;
  bool bool_value = false;
  std::string string_value;
  std::string enum_value;
  std::vector<std::string> enum_entries;
  std::vector<FeatureNode> children;
};

class NodeMap;

struct NodeMapAccess;

class Node {
public:
  std::string name() const;
  std::string display_name() const;
  std::string description() const;
  std::string tooltip() const;
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
  /** Returns the numeric value of the current enumeration entry. */
  std::int64_t get_enum_value() const;
  void set_enum(std::string_view entry);
  std::vector<std::string> enum_entries() const;

  void execute();

  std::string value_text() const;

  void set(std::int64_t v);
  void set(int v) { set(static_cast<std::int64_t>(v)); }
  void set(double v);
  void set(bool v);
  void set(std::string_view v);
  void set(const char* v) { set(std::string_view(v ? v : "")); }

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

/** Copy one node, including category children. */
FeatureNode snapshot(const Node& node);
/** Apply a copied write: int, float, bool, string, enum, or execute. */
void apply_feature(Node node, std::string_view op, std::string_view value);

}  // namespace onegen
