#pragma once

#include "OneGenNode.h"
#include "OneGenTypes.h"
#include "OneGenError.h"
#include <cmath>

namespace onegen {

/** Read SFNC calibration without inventing axes; restore a writable coordinate selector. */
inline std::optional<Frame::Scan3dGeometry> read_scan3d_geometry(
    NodeMap& features, std::string_view prefix,
    const Frame::Scan3dGeometry* baseline = nullptr)
{
  const auto named = [prefix](const char* suffix) { return std::string(prefix) + suffix; };
  auto selector = features.find(named("Scan3dCoordinateSelector"));
  auto scale = features.find(named("Scan3dCoordinateScale"));
  auto offset = features.find(named("Scan3dCoordinateOffset"));
  auto flag = features.find(named("Scan3dInvalidDataFlag"));
  auto invalid = features.find(named("Scan3dInvalidDataValue"));
  const auto text = [&](const char* suffix) {
    auto node = features.find(named(suffix));
    if (!node.is_readable()) return std::string{};
    return node.kind() == NodeKind::Enumeration ? node.get_enum() : node.get_string();
  };
  Frame::Scan3dGeometry geometry;
  geometry.calibrated_axes.fill(false);
  try {
    geometry.output_mode = text("Scan3dOutputMode");
    geometry.coordinate_system = text("Scan3dCoordinateSystem");
    geometry.distance_unit = text("Scan3dDistanceUnit");
    if (baseline) {
      if (geometry.output_mode.empty()) geometry.output_mode = baseline->output_mode;
      if (geometry.coordinate_system.empty()) geometry.coordinate_system = baseline->coordinate_system;
      if (geometry.distance_unit.empty()) geometry.distance_unit = baseline->distance_unit;
    }
  } catch (...) { return std::nullopt; }
  const auto optional_float = [&](const char* suffix, double& destination) {
    auto node = features.find(named(suffix));
    if (!node.is_readable()) return;
    try { destination = node.get_float(); } catch (...) {}
  };
  optional_float("Scan3dFocalLength", geometry.focal_length);
  optional_float("Scan3dBaseline", geometry.baseline);
  optional_float("Scan3dPrincipalPointU", geometry.principal_u);
  optional_float("Scan3dPrincipalPointV", geometry.principal_v);
  if (baseline) {
    if (!std::isfinite(geometry.focal_length)) geometry.focal_length = baseline->focal_length;
    if (!std::isfinite(geometry.baseline)) geometry.baseline = baseline->baseline;
    if (!std::isfinite(geometry.principal_u)) geometry.principal_u = baseline->principal_u;
    if (!std::isfinite(geometry.principal_v)) geometry.principal_v = baseline->principal_v;
  }
  const bool uncalibrated = geometry.output_mode == "UncalibratedC";
  if (!uncalibrated && (geometry.distance_unit.empty()
      || !scale.is_readable() || !offset.is_readable())) return std::nullopt;
  const bool c_only = geometry.output_mode == "CalibratedC" || uncalibrated
      || geometry.output_mode == "DisparityC";
  std::string previous;
  if (selector.valid()) {
    if (!selector.is_readable()) return std::nullopt;
    try { previous = selector.get_enum(); } catch (...) { return std::nullopt; }
  } else if (!c_only) {
    // CalibratedC and DisparityC identify unselected scalar calibration as coordinate C.
    return std::nullopt;
  }
  bool changed = false;
  bool c_read = false;
  const char* axes[] = {"CoordinateA", "CoordinateB", "CoordinateC"};
  for (std::size_t axis = c_only ? 2 : 0; axis < 3; ++axis) {
    try {
      if (selector.valid() && previous != axes[axis]) {
        if (!selector.is_writable()) continue;
        changed = true;
        selector.set_enum(axes[axis]);
      } else if (selector.valid() && changed) {
        selector.set_enum(axes[axis]);
      }
      if (!uncalibrated) {
        geometry.scale[axis] = scale.get_float();
        geometry.offset[axis] = offset.get_float();
      }
      if (!std::isfinite(geometry.scale[axis]) || !std::isfinite(geometry.offset[axis])) continue;
      if (flag.valid()) {
        if (!flag.is_readable()) continue;
        geometry.invalid_enabled[axis] = flag.kind() == NodeKind::Boolean
            ? flag.get_bool() : flag.get_int() != 0;
      } else if (baseline) {
        geometry.invalid_enabled[axis] = baseline->invalid_enabled[axis];
        geometry.invalid_value[axis] = baseline->invalid_value[axis];
      }
      if (geometry.invalid_enabled[axis] && (flag.valid() || !baseline)) {
        if (!invalid.is_readable()) continue;
        geometry.invalid_value[axis] = invalid.kind() == NodeKind::Float
            ? invalid.get_float() : static_cast<double>(invalid.get_int());
      }
      geometry.calibrated_axes[axis] = !uncalibrated;
      if (axis == 2) c_read = true;
    } catch (...) { /* Other axes can remain independently usable. */ }
  }
  if (changed) {
    try { selector.set_enum(previous); }
    catch (...) { throw Error(named("Scan3dCoordinateSelector") + " could not be restored"); }
  }
  return c_read ? std::optional<Frame::Scan3dGeometry>(std::move(geometry))
                                     : std::nullopt;
}

} // namespace onegen
