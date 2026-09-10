#pragma once

#include "gentl.hpp"
#include "onegen/node.hpp"

namespace onegen {

void load_remote_nodemap(NodeMap& map, Gentl& g, GenTL::PORT_HANDLE port);
void load_string_nodemap(NodeMap& map, const char* xml);

}  // namespace onegen
