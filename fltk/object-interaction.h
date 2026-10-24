// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <optional>

#include "object-math.h"
namespace fsv::gltf {
struct Hand {
  Pose grip, aim;
  bool valid = false, aimValid = false;
  float squeeze = 0;
};
class Manipulator {
  std::array<bool, 2> pressed{}, held{}, blocked{};
  int mask = 0;
  Pose base, anchor;
  Pose pair(const std::array<Hand, 2> &) const;
  bool hit(const Hand &, Pose, const Bounds &) const;

public:
  Pose object{{0, 0, -1.2f}, {}, .65f};
  void reset(Pose head);
  void release();
  void update(const std::array<Hand, 2> &, const Bounds &normalizedBounds,
              bool focused = true);
  int grabbing() const { return mask; }
};
} // namespace fsv::gltf
