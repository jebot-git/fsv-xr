// SPDX-License-Identifier: MIT
#pragma once
#include <memory>

#include "gltf-model.h"
#include "object-interaction.h"
namespace fsv::gltf {
class Renderer {
  struct Impl;
  std::unique_ptr<Impl> impl;

public:
  Renderer();
  ~Renderer();
  Renderer(const Renderer &) = delete;
  Renderer &operator=(const Renderer &) = delete;
  void abandon();
  void clear();
  void prepare(std::shared_ptr<const Model>, double seconds, int animation = 0);
  void draw(const Mat4 &viewProjection, const Mat4 &root, Vec3 eye, int width,
            int height,
            std::array<float, 4> background = {.035f, .045f, .06f, 1});
  void controllers(const Mat4 &viewProjection, const std::array<Hand, 2> &,
                   Pose object, const Bounds &, int grabbing);
  size_t uploadedVertices() const;
};
} // namespace fsv::gltf
