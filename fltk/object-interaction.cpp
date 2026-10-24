// SPDX-License-Identifier: MIT
#include "object-interaction.h"
namespace fsv::gltf {
Pose Manipulator::pair(const std::array<Hand, 2> &hands) const {
  auto a = hands[0].grip, b = hands[1].grip;
  auto x = normalized(b.position - a.position);
  auto up = rotate(a.orientation, {0, 1, 0}) + rotate(b.orientation, {0, 1, 0});
  auto y = normalized(up - x * dot(up, x), std::abs(x.y) < .9f
                                               ? normalized(cross({0, 0, 1}, x))
                                               : Vec3{1, 0, 0});
  auto z = normalized(cross(x, y));
  y = cross(z, x);
  return {(a.position + b.position) * .5f, basis(x, y, z),
          std::max(.03f, length(b.position - a.position))};
}
bool Manipulator::hit(const Hand &hand, Pose pose, const Bounds &bounds) const {
  if (!bounds.valid)
    return false;
  auto local = inverse(pose);
  auto p = transform(local, hand.grip.position);
  auto inside = [&](Vec3 v, float margin) {
    return v.x >= bounds.min.x - margin && v.x <= bounds.max.x + margin &&
           v.y >= bounds.min.y - margin && v.y <= bounds.max.y + margin &&
           v.z >= bounds.min.z - margin && v.z <= bounds.max.z + margin;
  };
  if (inside(p, .10f / pose.scale))
    return true;
  if (!hand.aimValid || !finite(hand.aim))
    return false;
  auto origin = transform(local, hand.aim.position);
  auto direction =
      rotate(local.orientation, rotate(hand.aim.orientation, {0, 0, -1}));
  float near = 0, far = 10 / pose.scale;
  const float o[] = {origin.x, origin.y, origin.z},
              d[] = {direction.x, direction.y, direction.z},
              lo[] = {bounds.min.x, bounds.min.y, bounds.min.z},
              hi[] = {bounds.max.x, bounds.max.y, bounds.max.z};
  for (int i = 0; i < 3; ++i) {
    if (std::abs(d[i]) < 1e-7) {
      if (o[i] < lo[i] || o[i] > hi[i])
        return false;
      continue;
    }
    float a = (lo[i] - o[i]) / d[i], b = (hi[i] - o[i]) / d[i];
    if (a > b)
      std::swap(a, b);
    near = std::max(near, a);
    far = std::min(far, b);
    if (far < near)
      return false;
  }
  return true;
}
void Manipulator::release() {
  for (int i = 0; i < 2; ++i) {
    blocked[i] = pressed[i];
    held[i] = false;
  }
  mask = 0;
}
void Manipulator::reset(Pose head) {
  release();
  object = {transform(head, {0, -.12f, -1.2f}), head.orientation, .65f};
}
void Manipulator::update(const std::array<Hand, 2> &hands, const Bounds &bounds,
                         bool focused) {
  int next = 0;
  for (int i = 0; i < 2; ++i) {
    auto &h = hands[i];
    bool down = std::isfinite(h.squeeze) &&
                (pressed[i] ? h.squeeze > .35f : h.squeeze > .7f);
    if (!down)
      blocked[i] = false;
    bool tracked = focused && h.valid && finite(h.grip);
    if (!tracked) {
      if (held[i])
        blocked[i] = true;
      held[i] = false;
    } else if (!down)
      held[i] = false;
    else if (!pressed[i] && !blocked[i] && hit(h, object, bounds))
      held[i] = true;
    pressed[i] = down;
    if (held[i])
      next |= 1 << i;
  }
  if (!next) {
    mask = 0;
    return;
  }
  Pose current = next == 3 ? pair(hands) : hands[next == 1 ? 0 : 1].grip;
  if (next != mask) {
    mask = next;
    base = object;
    anchor = current;
    return;
  }
  if (next == 3 &&
      length(hands[1].grip.position - hands[0].grip.position) < .03f)
    return;
  if (next == 3)
    current.scale =
        anchor.scale *
        std::clamp(base.scale * current.scale / anchor.scale, .02f, 20.f) /
        base.scale;
  auto delta = compose(current, inverse(anchor));
  auto result = compose(delta, base);
  if (finite(result))
    object = result;
}
} // namespace fsv::gltf
