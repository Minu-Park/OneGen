#pragma once

#include "Internal/Gentl.h"
#include "OneGenNode.h"

namespace onegen {

/** Loads a module XML and reports failures before preserving the original exception. */
void load_remote_nodemap(NodeMap& map, Gentl& g, GenTL::PORT_HANDLE port,
                         std::string_view module = {},
                         std::function<void(std::string_view)> diagnostic = {});
void load_string_nodemap(NodeMap& map, const char* xml);

}  // namespace onegen
