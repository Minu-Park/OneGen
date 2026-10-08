#pragma once
#include "OneGenWorkerMessages.h"
#include <optional>

namespace onegen::worker {
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
/** @brief Encodes a complete observation for same-request worker validation. */
inline void write(Writer& out, const NodeObservation& value) {
  write(out, value.node); out.scalar(value.stream_index); out.scalar(value.stream_generation);
}
/** @brief Decodes an owned observation without SDK handles. */
inline NodeObservation readNodeObservation(Reader& in) {
  NodeObservation result; result.node = readFeatureNode(in);
  result.stream_index = in.scalar<std::uint32_t>(); result.stream_generation = in.scalar<std::uint64_t>(); return result;
}
/** @brief Encodes an explicit result even when the SDK reports a post-effect failure. */
inline void write(Writer& out, const NodeWriteResult& value) {
  out.scalar(value.status); out.scalar(value.effect); out.string(value.message);
  out.scalar(bool(value.observation)); if (value.observation) write(out, *value.observation);
}
/** @brief Rejects invalid outcomes instead of interpreting them as successful effects. */
inline NodeWriteResult readNodeWriteResult(Reader& in) {
  NodeWriteResult result; result.status = in.scalar<NodeWriteStatus>(); result.effect = in.scalar<NodeWriteEffect>();
  if (result.status > NodeWriteStatus::BackendFailure || result.effect > NodeWriteEffect::Unknown) throw std::runtime_error("Invalid checked-write outcome.");
  result.message = in.string(); if (in.scalar<bool>()) result.observation = readNodeObservation(in); return result;
}
}
