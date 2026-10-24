// SPDX-License-Identifier: MIT
#pragma once
#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "object-math.h"
namespace fsv::gltf {
struct Vertex {
  Vec3 position, normal{0, 1, 0};
  std::array<float, 2> uv{}, uv1{};
  std::array<float, 4> color{1, 1, 1, 1};
};
struct Influence {
  std::array<unsigned, 8> joints{};
  std::array<float, 8> weights{};
};
struct Morph {
  std::vector<Vec3> positions, normals;
};
struct Primitive {
  std::vector<Vertex> vertices;
  std::vector<unsigned> indices;
  std::vector<Influence> influences;
  std::vector<Morph> morphs;
  int material = -1;
  unsigned mode = 4;
};
struct Mesh {
  std::vector<Primitive> primitives;
  std::vector<float> weights;
};
struct Image {
  int width = 0, height = 0;
  std::vector<unsigned char> rgba;
};
struct Texture {
  int image = -1, mag = 9729, min = 9987, wrapS = 10497, wrapT = 10497;
};
struct TextureSlot {
  int texture = -1, coord = 0;
  std::array<float, 9> transform{1, 0, 0, 0, 1, 0, 0, 0, 1};
  float strength = 1;
};
struct Material {
  std::array<float, 4> base{1, 1, 1, 1};
  Vec3 emissive{};
  float metallic = 1, roughness = 1, cutoff = .5f;
  int alpha = 0;
  bool doubleSided = false, unlit = false;
  TextureSlot color, normal, metalRough, emission, occlusion;
};
struct Node {
  int parent = -1, mesh = -1, skin = -1;
  Vec3 translation{}, scale{1, 1, 1};
  Quat rotation{};
  Mat4 matrix;
  bool hasMatrix = false, active = false;
  std::vector<float> weights;
};
struct Skin {
  std::vector<int> joints;
  std::vector<Mat4> inverseBind;
};
struct Channel {
  int node = -1, path = 0, interpolation = 0, width = 0;
  std::vector<float> times, values;
};
struct Animation {
  std::string name;
  float duration = 0;
  std::vector<Channel> channels;
};
struct Draw {
  const Primitive *primitive = nullptr;
  Mat4 world;
  std::vector<Vertex> vertices;
  int node = 0, primitiveIndex = 0;
  bool deformed = false;
};
struct Frame {
  std::vector<Draw> draws;
};
class Model {
public:
  std::filesystem::path path;
  std::vector<std::filesystem::path> dependencies;
  std::vector<Mesh> meshes;
  std::vector<Node> nodes;
  std::vector<Skin> skins;
  std::vector<Material> materials;
  std::vector<Image> images;
  std::vector<Texture> textures;
  std::vector<Animation> animations;
  Bounds bounds;
  std::vector<std::string> warnings;
  Frame evaluate(double seconds, int animation = 0) const;
  Bounds normalizedBounds() const;
  size_t triangleCount() const;
};
bool supported(const std::filesystem::path &);
std::shared_ptr<Model> load(const std::filesystem::path &,
                            std::shared_ptr<std::atomic_bool> cancelled = {});
} // namespace fsv::gltf
