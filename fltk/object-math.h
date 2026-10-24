// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
namespace fsv::gltf {
struct Vec3 {
  float x = 0, y = 0, z = 0;
};
inline Vec3 operator+(Vec3 a, Vec3 b) {
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline Vec3 operator-(Vec3 a, Vec3 b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline Vec3 operator*(Vec3 a, float v) { return {a.x * v, a.y * v, a.z * v}; }
inline Vec3 operator/(Vec3 a, float v) { return a * (1 / v); }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }
inline bool finite(Vec3 a) {
  return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z);
}
inline Vec3 normalized(Vec3 a, Vec3 fallback = {1, 0, 0}) {
  float n = length(a);
  return n > 1e-7f ? a / n : fallback;
}
struct Quat {
  float x = 0, y = 0, z = 0, w = 1;
};
inline bool finite(Quat q) {
  return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) &&
         std::isfinite(q.w);
}
inline Quat normalized(Quat q) {
  float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  return n > 1e-7f ? Quat{q.x / n, q.y / n, q.z / n, q.w / n} : Quat{};
}
inline Quat conjugate(Quat q) {
  q = normalized(q);
  return {-q.x, -q.y, -q.z, q.w};
}
inline Quat operator*(Quat a, Quat b) {
  return normalized({a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                     a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                     a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                     a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z});
}
inline Vec3 rotate(Quat q, Vec3 v) {
  q = normalized(q);
  Vec3 a{q.x, q.y, q.z};
  return v + cross(a, v) * (2 * q.w) + cross(a, cross(a, v)) * 2;
}
inline Quat slerp(Quat a, Quat b, float t) {
  a = normalized(a);
  b = normalized(b);
  float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
  if (d < 0) {
    b = {-b.x, -b.y, -b.z, -b.w};
    d = -d;
  }
  float x = 1 - t, y = t;
  if (d < .9995f) {
    float angle = std::acos(std::clamp(d, -1.f, 1.f)), n = std::sin(angle);
    x = std::sin((1 - t) * angle) / n;
    y = std::sin(t * angle) / n;
  }
  return normalized({a.x * x + b.x * y, a.y * x + b.y * y, a.z * x + b.z * y,
                     a.w * x + b.w * y});
}
struct Mat4 {
  std::array<float, 16> v{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  const float *data() const { return v.data(); }
  float &operator[](size_t i) { return v[i]; }
  float operator[](size_t i) const { return v[i]; }
};
inline Mat4 operator*(const Mat4 &a, const Mat4 &b) {
  Mat4 r;
  r.v.fill(0);
  for (int c = 0; c < 4; ++c)
    for (int row = 0; row < 4; ++row)
      for (int k = 0; k < 4; ++k)
        r[c * 4 + row] += a[k * 4 + row] * b[c * 4 + k];
  return r;
}
inline Vec3 point(const Mat4 &m, Vec3 p) {
  float w = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
  if (std::abs(w) < 1e-8f)
    w = 1;
  return {(m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12]) / w,
          (m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13]) / w,
          (m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]) / w};
}
inline Vec3 vector(const Mat4 &m, Vec3 p) {
  return {m[0] * p.x + m[4] * p.y + m[8] * p.z,
          m[1] * p.x + m[5] * p.y + m[9] * p.z,
          m[2] * p.x + m[6] * p.y + m[10] * p.z};
}
inline Mat4 inverse(const Mat4 &m) {
  double a[4][8]{};
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      a[r][c] = m[c * 4 + r];
      a[r][c + 4] = r == c;
    }
  for (int c = 0; c < 4; ++c) {
    int p = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::abs(a[r][c]) > std::abs(a[p][c]))
        p = r;
    if (std::abs(a[p][c]) < 1e-12)
      throw std::runtime_error("Singular model transform");
    for (int j = 0; j < 8; ++j)
      std::swap(a[c][j], a[p][j]);
    double n = a[c][c];
    for (double &v : a[c])
      v /= n;
    for (int r = 0; r < 4; ++r)
      if (r != c) {
        n = a[r][c];
        for (int j = 0; j < 8; ++j)
          a[r][j] -= n * a[c][j];
      }
  }
  Mat4 result;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c)
      result[c * 4 + r] = a[r][c + 4];
  return result;
}
inline Mat4 trs(Vec3 t, Quat q, Vec3 s = {1, 1, 1}) {
  q = normalized(q);
  float x = q.x, y = q.y, z = q.z, w = q.w;
  Mat4 m;
  m.v = {(1 - 2 * y * y - 2 * z * z) * s.x,
         (2 * x * y + 2 * w * z) * s.x,
         (2 * x * z - 2 * w * y) * s.x,
         0,
         (2 * x * y - 2 * w * z) * s.y,
         (1 - 2 * x * x - 2 * z * z) * s.y,
         (2 * y * z + 2 * w * x) * s.y,
         0,
         (2 * x * z + 2 * w * y) * s.z,
         (2 * y * z - 2 * w * x) * s.z,
         (1 - 2 * x * x - 2 * y * y) * s.z,
         0,
         t.x,
         t.y,
         t.z,
         1};
  return m;
}
inline Quat basis(Vec3 x, Vec3 y, Vec3 z) {
  float trace = x.x + y.y + z.z;
  Quat q;
  if (trace > 0) {
    float s = std::sqrt(trace + 1) * 2;
    q = {(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, s / 4};
  } else if (x.x > y.y && x.x > z.z) {
    float s = std::sqrt(1 + x.x - y.y - z.z) * 2;
    q = {s / 4, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s};
  } else if (y.y > z.z) {
    float s = std::sqrt(1 + y.y - x.x - z.z) * 2;
    q = {(y.x + x.y) / s, s / 4, (z.y + y.z) / s, (z.x - x.z) / s};
  } else {
    float s = std::sqrt(1 + z.z - x.x - y.y) * 2;
    q = {(z.x + x.z) / s, (z.y + y.z) / s, s / 4, (x.y - y.x) / s};
  }
  return normalized(q);
}
struct Pose {
  Vec3 position;
  Quat orientation;
  float scale = 1;
};
inline bool finite(Pose p) {
  return finite(p.position) && finite(p.orientation) &&
         std::isfinite(p.scale) && p.scale > 0;
}
inline Vec3 transform(Pose p, Vec3 v) {
  return p.position + rotate(p.orientation, v * p.scale);
}
inline Pose compose(Pose a, Pose b) {
  return {transform(a, b.position), a.orientation * b.orientation,
          a.scale * b.scale};
}
inline Pose inverse(Pose p) {
  auto q = conjugate(p.orientation);
  return {rotate(q, p.position) * (-1 / p.scale), q, 1 / p.scale};
}
inline Mat4 matrix(Pose p) {
  return trs(p.position, p.orientation, {p.scale, p.scale, p.scale});
}
inline Mat4 perspective(float left, float right, float down, float up,
                        float near = .02f, float far = 100.f) {
  float l = std::tan(left), r = std::tan(right), d = std::tan(down),
        u = std::tan(up);
  Mat4 m;
  m.v.fill(0);
  m[0] = 2 / (r - l);
  m[5] = 2 / (u - d);
  m[8] = (r + l) / (r - l);
  m[9] = (u + d) / (u - d);
  m[10] = -(far + near) / (far - near);
  m[11] = -1;
  m[14] = -2 * far * near / (far - near);
  return m;
}
struct Bounds {
  Vec3 min{}, max{};
  bool valid = false;
  void include(Vec3 p) {
    if (!finite(p))
      throw std::runtime_error("Non-finite glTF position");
    if (!valid) {
      min = max = p;
      valid = true;
    } else {
      min = {std::min(min.x, p.x), std::min(min.y, p.y), std::min(min.z, p.z)};
      max = {std::max(max.x, p.x), std::max(max.y, p.y), std::max(max.z, p.z)};
    }
  }
  Vec3 center() const { return (min + max) * .5f; }
  Vec3 extent() const { return max - min; }
  Mat4 fit() const {
    auto e = extent();
    float n = std::max({e.x, e.y, e.z, 1e-6f});
    return trs(center() * (-1 / n), {}, {1 / n, 1 / n, 1 / n});
  }
};
} // namespace fsv::gltf
