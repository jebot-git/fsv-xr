// SPDX-License-Identifier: MIT
#pragma once
#include "object-math.h"
namespace fsv_stereo {
// The compositor's unchanged cylindrical window, including its focus outset.
struct CylinderSurface {
  fsv::gltf::Pose frame;
  float radius = 2, horizontal = 1, top = .5f, bottom = -.5f;
  fsv::gltf::Vec3 point(float u, float v) const {
    float angle = (u - .5f) * horizontal;
    return fsv::gltf::transform(frame, {radius * std::sin(angle),
                                        radius * (top + (bottom - top) * v),
                                        -radius * std::cos(angle)});
  }
  fsv::gltf::Pose tangent(float u, float v) const {
    float half = -(u - .5f) * horizontal / 2;
    return {point(u, v),
            frame.orientation *
                fsv::gltf::Quat{0, std::sin(half), 0, std::cos(half)},
            1};
  }
};
// Parallel cameras with an asymmetric frustum. The application surface is the
// zero-disparity plane; geometry behind it has uncrossed binocular disparity.
inline fsv::gltf::Mat4 windowEyeProjection(fsv::gltf::Vec3 eye, float width,
                                           float height, float distance) {
  using namespace fsv::gltf;
  float z = distance + eye.z;
  if (!finite(eye) || !std::isfinite(width) || !std::isfinite(height) ||
      !std::isfinite(distance) || width <= 0 || height <= 0 || z <= .02f)
    throw std::runtime_error("Invalid stereo window frustum");
  return perspective(std::atan((-width / 2 - eye.x) / z),
                     std::atan((width / 2 - eye.x) / z),
                     std::atan((-height / 2 - eye.y) / z),
                     std::atan((height / 2 - eye.y) / z)) *
         matrix(inverse(fsv::gltf::Pose{{eye.x, 1.6f + eye.y, eye.z}, {}, 1}));
}
} // namespace fsv_stereo
