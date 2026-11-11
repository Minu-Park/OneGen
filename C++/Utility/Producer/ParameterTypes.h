#pragma once

/** @file ParameterTypes.h @brief Copied parameter observations and checked-write outcomes. */

#include "OneGenNode.h"

#include <cstdint>
#include <optional>
#include <string>

namespace onegen::producer {

/** @brief Copies one feature with complete selector context and selected stream generation. */
struct NodeObservation {
  FeatureNode node;
  std::uint32_t stream_index = 0;
  std::uint64_t stream_generation = 0;
};

/** @brief Distinguishes pre-effect refusal from failed observation or uncertain mutation. */
enum class NodeWriteStatus : std::uint8_t { Success, Conflict, Unavailable, BackendFailure };

/** @brief Reports whether the existing SDK setter completed. */
enum class NodeWriteEffect : std::uint8_t { None, Applied, Unknown };

/** @brief Carries copied post-preparation readback and an explicit mutation outcome. */
struct NodeWriteResult {
  NodeWriteStatus status = NodeWriteStatus::Success;
  NodeWriteEffect effect = NodeWriteEffect::None;
  std::string message;
  std::optional<NodeObservation> observation;
};

}  // namespace onegen::producer
