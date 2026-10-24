// SPDX-License-Identifier: MIT
#include "gltf-model.h"
#define CGLTF_IMPLEMENTATION
#include "../third_party/cgltf/cgltf.h"
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 8192
#define STB_IMAGE_IMPLEMENTATION
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <set>

#include "../third_party/stb/stb_image.h"
namespace fsv::gltf {
namespace {
constexpr size_t memoryLimit = 384 * 1024 * 1024,
                 bufferLimit = 128 * 1024 * 1024, vertexLimit = 2000000,
                 indexLimit = 6000000;
struct Budget {
  std::shared_ptr<std::atomic_bool> cancelled;
  size_t live = 0;
  std::vector<std::filesystem::path> dependencies;
};
struct alignas(std::max_align_t) Block {
  size_t bytes;
};
void *allocate(void *user, size_t n) {
  auto &b = *static_cast<Budget *>(user);
  if ((b.cancelled && b.cancelled->load()) || n > memoryLimit - b.live ||
      n > SIZE_MAX - sizeof(Block))
    return nullptr;
  auto p = static_cast<Block *>(std::malloc(n + sizeof(Block)));
  if (!p)
    return nullptr;
  p->bytes = n;
  b.live += n;
  return p + 1;
}
void release(void *user, void *ptr) {
  if (ptr) {
    auto p = static_cast<Block *>(ptr) - 1;
    static_cast<Budget *>(user)->live -= p->bytes;
    std::free(p);
  }
}
cgltf_result readFile(const cgltf_memory_options *memory,
                      const cgltf_file_options *files, const char *path,
                      cgltf_size *size, void **data) {
  int fd = -1;
  void *buffer = nullptr;
  try {
    auto &budget = *static_cast<Budget *>(files->user_data);
    auto file = std::filesystem::weakly_canonical(path);
    budget.dependencies.push_back(file);
    fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
      return cgltf_result_file_not_found;
    struct stat info{};
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        uint64_t(info.st_size) > bufferLimit ||
        (*size && *size > uint64_t(info.st_size))) {
      ::close(fd);
      return cgltf_result_io_error;
    }
    if (!*size)
      *size = info.st_size;
    if (*size > bufferLimit) {
      ::close(fd);
      return cgltf_result_out_of_memory;
    }
    buffer = memory->alloc_func(memory->user_data, *size);
    if (!buffer) {
      ::close(fd);
      return cgltf_result_out_of_memory;
    }
    size_t offset = 0;
    while (offset < *size) {
      if (budget.cancelled && budget.cancelled->load())
        throw std::runtime_error("Cancelled glTF read");
      auto n = ::read(fd, static_cast<char *>(buffer) + offset,
                      std::min(size_t(65536), *size - offset));
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        throw std::runtime_error("Short glTF resource read");
      offset += n;
    }
    ::close(fd);
    *data = buffer;
    return cgltf_result_success;
  } catch (...) {
    if (fd >= 0)
      ::close(fd);
    if (buffer)
      memory->free_func(memory->user_data, buffer);
    return cgltf_result_io_error;
  }
}
void releaseFile(const cgltf_memory_options *memory, const cgltf_file_options *,
                 void *p, cgltf_size) {
  memory->free_func(memory->user_data, p);
}
void require(bool okay, const std::string &what) {
  if (!okay)
    throw std::runtime_error(what);
}
void check(cgltf_result result, const char *what) {
  if (result != cgltf_result_success)
    throw std::runtime_error(std::string(what) + " (glTF error " +
                             std::to_string(result) + ")");
}
std::filesystem::path external(const std::filesystem::path &base,
                               const char *uri) {
  std::string value = uri ? uri : "";
  require(value.find(':') == std::string::npos,
          "Network/absolute URI resources are not supported in previews");
  std::vector<char> decoded(value.begin(), value.end());
  decoded.push_back(0);
  cgltf_decode_uri(decoded.data());
  std::filesystem::path relative(decoded.data());
  require(!relative.is_absolute(), "glTF resource must use a relative URI");
  return base.parent_path() / relative;
}
std::vector<float> unpack(const cgltf_accessor *a, size_t width, size_t count) {
  require(a && a->count == count && cgltf_num_components(a->type) == width,
          "Invalid glTF attribute shape");
  require(count <= indexLimit && width <= 64,
          "glTF accessor exceeds preview limits");
  std::vector<float> values(count * width);
  require(cgltf_accessor_unpack_floats(a, values.data(), values.size()) ==
              values.size(),
          "Cannot decode glTF accessor");
  for (float v : values)
    require(std::isfinite(v), "Non-finite glTF attribute");
  return values;
}
const cgltf_accessor *attribute(const cgltf_primitive &p,
                                cgltf_attribute_type type, int index = 0) {
  for (size_t i = 0; i < p.attributes_count; ++i)
    if (p.attributes[i].type == type && p.attributes[i].index == index)
      return p.attributes[i].data;
  return nullptr;
}
const cgltf_accessor *attribute(const cgltf_morph_target &p,
                                cgltf_attribute_type type) {
  for (size_t i = 0; i < p.attributes_count; ++i)
    if (p.attributes[i].type == type)
      return p.attributes[i].data;
  return nullptr;
}
Vec3 vec(const float *v) { return {v[0], v[1], v[2]}; }
std::vector<unsigned char> imageBytes(const cgltf_image &image,
                                      const std::filesystem::path &path,
                                      Budget &budget, cgltf_options &options) {
  if (image.buffer_view) {
    auto ptr = cgltf_buffer_view_data(image.buffer_view);
    require(ptr, "Missing glTF image buffer");
    require(image.buffer_view->size <= 32 * 1024 * 1024,
            "glTF image exceeds 32 MiB");
    return {ptr, ptr + image.buffer_view->size};
  }
  require(image.uri, "Missing glTF image URI");
  std::string uri = image.uri;
  if (uri.rfind("data:", 0) == 0) {
    auto comma = uri.find(',');
    require(comma != uri.npos &&
                uri.substr(0, comma).find(";base64") != uri.npos,
            "Unsupported image data URI");
    auto base64 = uri.substr(comma + 1);
    size_t padding = base64.empty() ? 0 : base64.back() == '=' ? 1 : 0;
    if (base64.size() > 1 && base64[base64.size() - 2] == '=')
      ++padding;
    require(base64.size() % 4 == 0 && base64.size() <= 48 * 1024 * 1024,
            "Invalid or oversized image data URI");
    size_t n = base64.size() / 4 * 3 - padding;
    void *bytes = nullptr;
    check(cgltf_load_buffer_base64(&options, n, base64.c_str(), &bytes),
          "Cannot decode image URI");
    auto deleter = [&](void *p) { release(&budget, p); };
    std::unique_ptr<void, decltype(deleter)> owned(bytes, deleter);
    return {static_cast<unsigned char *>(bytes),
            static_cast<unsigned char *>(bytes) + n};
  }
  auto file = external(path, image.uri);
  size_t size = 0;
  void *bytes = nullptr;
  check(readFile(&options.memory, &options.file, file.c_str(), &size, &bytes),
        "Cannot read glTF texture");
  auto deleter = [&](void *p) { release(&budget, p); };
  std::unique_ptr<void, decltype(deleter)> owned(bytes, deleter);
  require(size <= 32 * 1024 * 1024, "glTF image exceeds 32 MiB");
  return {static_cast<unsigned char *>(bytes),
          static_cast<unsigned char *>(bytes) + size};
}
TextureSlot slot(const cgltf_texture_view &v, const cgltf_data &d) {
  TextureSlot out;
  if (!v.texture)
    return out;
  out.texture = int(v.texture - d.textures);
  out.coord = v.texcoord;
  out.strength = v.scale;
  if (v.has_transform) {
    auto &t = v.transform;
    if (t.has_texcoord)
      out.coord = t.texcoord;
    float c = std::cos(t.rotation), s = std::sin(t.rotation);
    out.transform = {c * t.scale[0],  s * t.scale[0], 0,
                     -s * t.scale[1], c * t.scale[1], 0,
                     t.offset[0],     t.offset[1],    1};
  }
  require(out.coord >= 0 && out.coord <= 1,
          "Preview supports texture coordinates TEXCOORD_0 and TEXCOORD_1");
  return out;
}
} // namespace
bool supported(const std::filesystem::path &path) {
  auto ext = path.extension().string();
  for (auto &c : ext)
    c = char(std::tolower(static_cast<unsigned char>(c)));
  return ext == ".gltf" || ext == ".glb";
}
std::shared_ptr<Model> load(const std::filesystem::path &input,
                            std::shared_ptr<std::atomic_bool> cancelled) {
  auto model = std::make_shared<Model>();
  model->path = std::filesystem::canonical(input);
  Budget budget;
  budget.cancelled = std::move(cancelled);
  cgltf_options options{};
  options.memory = {allocate, release, &budget};
  options.file = {readFile, releaseFile, &budget};
  cgltf_data *raw = nullptr;
  check(cgltf_parse_file(&options, model->path.c_str(), &raw),
        "Cannot parse glTF");
  std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data(raw, cgltf_free);
  auto &d = *raw;
  require(d.nodes_count <= 32768 && d.meshes_count <= 16384 &&
              d.materials_count <= 4096 && d.images_count <= 256 &&
              d.animations_count <= 128,
          "glTF scene exceeds preview limits");
  static const std::set<std::string> supportedExtensions = {
      "KHR_materials_unlit", "KHR_texture_transform", "KHR_mesh_quantization",
      "KHR_materials_emissive_strength", "KHR_materials_pbrSpecularGlossiness"};
  for (size_t i = 0; i < d.extensions_required_count; ++i)
    require(supportedExtensions.count(d.extensions_required[i]),
            std::string("Required glTF extension is not supported: ") +
                d.extensions_required[i]);
  size_t buffers = 0;
  for (size_t i = 0; i < d.buffers_count; ++i) {
    require(d.buffers[i].size <= bufferLimit - buffers,
            "glTF buffers exceed 128 MiB");
    buffers += d.buffers[i].size;
    if (d.buffers[i].uri && strncmp(d.buffers[i].uri, "data:", 5))
      external(model->path, d.buffers[i].uri);
  }
  check(cgltf_load_buffers(&options, &d, model->path.c_str()),
        "Cannot load glTF buffers");
  check(cgltf_validate(&d), "Invalid glTF scene");
  size_t decodedBytes = 0;
  auto charge = [&](size_t n) {
    require(n <= 256 * 1024 * 1024 - decodedBytes,
            "Decoded glTF scene exceeds 256 MiB");
    decodedBytes += n;
  };
  size_t decodedImages = 0;
  for (size_t i = 0; i < d.images_count; ++i) {
    auto bytes = imageBytes(d.images[i], model->path, budget, options);
    int w = 0, h = 0, channels = 0;
    require(stbi_info_from_memory(bytes.data(), bytes.size(), &w, &h,
                                  &channels) != 0,
            "Cannot inspect PNG/JPEG glTF texture");
    require(w > 0 && h > 0 && w <= 8192 && h <= 8192 &&
                size_t(w) * h <= 16 * 1024 * 1024,
            "glTF image dimensions exceed preview limits");
    size_t n = size_t(w) * h * 4;
    require(n <= 128 * 1024 * 1024 - decodedImages,
            "Decoded glTF textures exceed 128 MiB");
    decodedImages += n;
    charge(n);
    auto rgba =
        stbi_load_from_memory(bytes.data(), bytes.size(), &w, &h, &channels, 4);
    require(rgba, "Cannot decode glTF texture");
    std::unique_ptr<unsigned char, decltype(&stbi_image_free)> owned(
        rgba, stbi_image_free);
    Image image;
    image.width = w;
    image.height = h;
    image.rgba.assign(rgba, rgba + n);
    model->images.push_back(std::move(image));
  }
  for (size_t i = 0; i < d.textures_count; ++i) {
    auto &texture = d.textures[i];
    Texture out;
    out.image = texture.image ? int(texture.image - d.images) : -1;
    require(out.image >= 0, "Compressed or alternate texture requires an "
                            "unsupported decoder");
    if (texture.sampler) {
      out.mag =
          texture.sampler->mag_filter ? texture.sampler->mag_filter : 9729;
      out.min =
          texture.sampler->min_filter ? texture.sampler->min_filter : 9987;
      out.wrapS = texture.sampler->wrap_s;
      out.wrapT = texture.sampler->wrap_t;
    }
    model->textures.push_back(out);
  }
  for (size_t i = 0; i < d.materials_count; ++i) {
    auto &m = d.materials[i];
    Material out;
    out.doubleSided = m.double_sided;
    out.unlit = m.unlit;
    out.alpha = int(m.alpha_mode);
    out.cutoff = m.alpha_cutoff;
    out.emissive = vec(m.emissive_factor);
    if (m.has_emissive_strength)
      out.emissive = out.emissive * m.emissive_strength.emissive_strength;
    auto &p = m.pbr_metallic_roughness;
    std::copy(p.base_color_factor, p.base_color_factor + 4, out.base.begin());
    out.metallic = p.metallic_factor;
    out.roughness = p.roughness_factor;
    out.color = slot(p.base_color_texture, d);
    out.metalRough = slot(p.metallic_roughness_texture, d);
    if (m.has_pbr_specular_glossiness) {
      auto &s = m.pbr_specular_glossiness;
      std::copy(s.diffuse_factor, s.diffuse_factor + 4, out.base.begin());
      out.metallic = 0;
      out.roughness = 1 - s.glossiness_factor;
      out.color = slot(s.diffuse_texture, d);
      model->warnings.push_back("Specular-glossiness material approximated as "
                                "metallic-roughness");
    }
    out.normal = slot(m.normal_texture, d);
    out.emission = slot(m.emissive_texture, d);
    out.occlusion = slot(m.occlusion_texture, d);
    model->materials.push_back(out);
  }
  size_t totalVertices = 0, totalIndices = 0, totalMorphValues = 0;
  for (size_t mi = 0; mi < d.meshes_count; ++mi) {
    auto &source = d.meshes[mi];
    Mesh mesh;
    if (source.weights_count)
      mesh.weights.assign(source.weights,
                          source.weights + source.weights_count);
    for (size_t pi = 0; pi < source.primitives_count; ++pi) {
      auto &p = source.primitives[pi];
      require(!budget.cancelled || !budget.cancelled->load(),
              "Cancelled glTF load");
      require(!p.has_draco_mesh_compression,
              "Draco-compressed glTF needs decompression "
              "before preview");
      auto position = attribute(p, cgltf_attribute_type_position);
      require(position, "glTF primitive has no positions");
      size_t count = position->count;
      require(count <= vertexLimit - totalVertices,
              "glTF exceeds two million vertices");
      totalVertices += count;
      charge(count * sizeof(Vertex));
      Primitive out;
      out.vertices.resize(count);
      out.material = p.material ? int(p.material - d.materials) : -1;
      out.mode = unsigned(p.type) - 1;
      auto positions = unpack(position, 3, count);
      for (size_t i = 0; i < count; ++i)
        out.vertices[i].position = vec(&positions[i * 3]);
      auto normals = attribute(p, cgltf_attribute_type_normal);
      if (normals) {
        auto values = unpack(normals, 3, count);
        for (size_t i = 0; i < count; ++i)
          out.vertices[i].normal = normalized(vec(&values[i * 3]), {0, 1, 0});
      }
      for (int set = 0; set < 2; ++set)
        if (auto a = attribute(p, cgltf_attribute_type_texcoord, set)) {
          auto values = unpack(a, 2, count);
          for (size_t i = 0; i < count; ++i)
            (set ? out.vertices[i].uv1 : out.vertices[i].uv) =
                std::array<float, 2>{values[i * 2], values[i * 2 + 1]};
        }
      if (auto a = attribute(p, cgltf_attribute_type_color)) {
        size_t n = cgltf_num_components(a->type);
        require(n == 3 || n == 4, "Invalid glTF vertex color");
        auto values = unpack(a, n, count);
        for (size_t i = 0; i < count; ++i)
          for (size_t j = 0; j < n; ++j)
            out.vertices[i].color[j] = values[i * n + j];
      }
      size_t indices = p.indices ? p.indices->count : count;
      require(indices <= indexLimit - totalIndices,
              "glTF index count exceeds preview limits");
      out.indices.resize(indices);
      std::vector<float> unpacked;
      if (p.indices)
        unpacked = unpack(p.indices, 1, indices);
      for (size_t i = 0; i < indices; ++i) {
        float index = p.indices ? unpacked[i] : float(i);
        require(index >= 0 && index < count && std::floor(index) == index,
                "glTF index is outside vertex buffer");
        out.indices[i] = unsigned(index);
      }
      if (out.mode == 5 || out.mode == 6) {
        std::vector<unsigned> triangles;
        require(indices <= indexLimit / 3, "glTF strip exceeds preview limits");
        for (size_t i = 2; i < indices; ++i) {
          if (out.mode == 6) {
            triangles.insert(
                triangles.end(),
                {out.indices[0], out.indices[i - 1], out.indices[i]});
          } else if (i % 2) {
            triangles.insert(
                triangles.end(),
                {out.indices[i - 1], out.indices[i - 2], out.indices[i]});
          } else
            triangles.insert(
                triangles.end(),
                {out.indices[i - 2], out.indices[i - 1], out.indices[i]});
        }
        out.indices = std::move(triangles);
        out.mode = 4;
      }
      require(out.mode <= 4, "Unsupported glTF primitive mode");
      require(out.indices.size() <= indexLimit - totalIndices,
              "glTF exceeds six million indices");
      totalIndices += out.indices.size();
      charge(out.indices.size() * sizeof(unsigned));
      if (out.mode == 4) {
        require(out.indices.size() % 3 == 0, "Invalid triangle index count");
        if (!normals) {
          for (auto &v : out.vertices)
            v.normal = {};
          for (size_t i = 0; i < out.indices.size(); i += 3) {
            auto a = out.indices[i], b = out.indices[i + 1],
                 c = out.indices[i + 2];
            auto n = cross(out.vertices[b].position - out.vertices[a].position,
                           out.vertices[c].position - out.vertices[a].position);
            for (auto index : {a, b, c})
              out.vertices[index].normal = out.vertices[index].normal + n;
          }
          for (auto &v : out.vertices)
            v.normal = normalized(v.normal, {0, 1, 0});
        }
      }
      for (int set = 0; set < 2; ++set) {
        auto joints = attribute(p, cgltf_attribute_type_joints, set),
             weights = attribute(p, cgltf_attribute_type_weights, set);
        if (joints || weights) {
          require(joints && weights, "glTF skin attributes must be "
                                     "paired");
          auto j = unpack(joints, 4, count), w = unpack(weights, 4, count);
          if (out.influences.empty()) {
            charge(count * sizeof(Influence));
            out.influences.resize(count);
          }
          for (size_t i = 0; i < count; ++i)
            for (int k = 0; k < 4; ++k) {
              require(j[i * 4 + k] >= 0 && j[i * 4 + k] < 65536 &&
                          std::floor(j[i * 4 + k]) == j[i * 4 + k] &&
                          w[i * 4 + k] >= 0,
                      "Invalid glTF skin "
                      "weights");
              out.influences[i].joints[set * 4 + k] = unsigned(j[i * 4 + k]);
              out.influences[i].weights[set * 4 + k] = w[i * 4 + k];
            }
        }
      }
      require(p.targets_count <= 64, "Too many glTF morph targets");
      for (size_t target = 0; target < p.targets_count; ++target) {
        Morph morph;
        for (auto type :
             {cgltf_attribute_type_position, cgltf_attribute_type_normal})
          if (auto a = attribute(p.targets[target], type)) {
            require(count <= 8000000 - totalMorphValues,
                    "glTF morph data exceeds "
                    "preview limits");
            totalMorphValues += count;
            charge(count * sizeof(Vec3));
            auto values = unpack(a, 3, count);
            auto &v = type == cgltf_attribute_type_position ? morph.positions
                                                            : morph.normals;
            v.resize(count);
            for (size_t i = 0; i < count; ++i)
              v[i] = vec(&values[i * 3]);
          }
        out.morphs.push_back(std::move(morph));
      }
      mesh.primitives.push_back(std::move(out));
    }
    model->meshes.push_back(std::move(mesh));
  }
  for (size_t i = 0; i < d.nodes_count; ++i) {
    auto &n = d.nodes[i];
    Node out;
    out.parent = n.parent ? int(n.parent - d.nodes) : -1;
    out.mesh = n.mesh ? int(n.mesh - d.meshes) : -1;
    out.skin = n.skin ? int(n.skin - d.skins) : -1;
    out.translation = vec(n.translation);
    out.scale = vec(n.scale);
    out.rotation = {n.rotation[0], n.rotation[1], n.rotation[2], n.rotation[3]};
    out.hasMatrix = n.has_matrix;
    std::copy(n.matrix, n.matrix + 16, out.matrix.v.begin());
    if (out.hasMatrix)
      for (float v : out.matrix.v)
        require(std::isfinite(v), "Non-finite glTF node matrix");
    if (n.weights_count)
      out.weights.assign(n.weights, n.weights + n.weights_count);
    else if (out.mesh >= 0)
      out.weights = model->meshes[out.mesh].weights;
    require(finite(out.translation) && finite(out.rotation) &&
                finite(out.scale),
            "Non-finite glTF node transform");
    model->nodes.push_back(std::move(out));
  }
  std::vector<int> active;
  auto selected = d.scene ? d.scene : d.scenes_count ? &d.scenes[0] : nullptr;
  if (selected) {
    for (size_t i = 0; i < selected->nodes_count; ++i)
      active.push_back(int(selected->nodes[i] - d.nodes));
  } else
    for (size_t i = 0; i < d.nodes_count; ++i)
      if (!d.nodes[i].parent)
        active.push_back(i);
  for (size_t i = 0; i < active.size(); ++i) {
    int n = active[i];
    require(!model->nodes[n].active, "Cyclic or repeated glTF scene node");
    model->nodes[n].active = true;
    for (size_t c = 0; c < d.nodes[n].children_count; ++c)
      active.push_back(int(d.nodes[n].children[c] - d.nodes));
    require(active.size() <= d.nodes_count, "Cyclic glTF node hierarchy");
  }
  for (size_t i = 0; i < d.skins_count; ++i) {
    auto &s = d.skins[i];
    require(s.joints_count <= 512, "glTF skeleton exceeds 512 joints");
    charge(s.joints_count * (sizeof(Mat4) + sizeof(int)));
    Skin out;
    out.inverseBind.resize(s.joints_count);
    for (size_t j = 0; j < s.joints_count; ++j)
      out.joints.push_back(int(s.joints[j] - d.nodes));
    if (s.inverse_bind_matrices) {
      auto values = unpack(s.inverse_bind_matrices, 16, s.joints_count);
      for (size_t j = 0; j < s.joints_count; ++j)
        std::copy(values.begin() + j * 16, values.begin() + (j + 1) * 16,
                  out.inverseBind[j].v.begin());
    }
    model->skins.push_back(std::move(out));
  }
  size_t animationValues = 0, animationChannels = 0;
  for (size_t i = 0; i < d.animations_count; ++i) {
    auto &a = d.animations[i];
    Animation out;
    out.name = a.name ? a.name : "Animation " + std::to_string(i + 1);
    for (size_t c = 0; c < a.channels_count; ++c) {
      auto &channel = a.channels[c];
      require(++animationChannels <= 4096, "Too many animation channels");
      require(channel.target_node && channel.sampler,
              "Invalid glTF animation target");
      Channel result;
      result.node = int(channel.target_node - d.nodes);
      result.path = int(channel.target_path);
      result.interpolation = int(channel.sampler->interpolation);
      size_t count = channel.sampler->input->count;
      require(count > 0, "Empty glTF animation sampler");
      result.times = unpack(channel.sampler->input, 1, count);
      for (size_t k = 0; k < count; ++k) {
        require(result.times[k] >= 0 &&
                    (!k || result.times[k] > result.times[k - 1]),
                "Animation key times must increase");
      }
      out.duration = std::max(out.duration, result.times.back());
      if (channel.target_path == cgltf_animation_path_type_weights) {
        auto node = channel.target_node;
        require(node->mesh && node->mesh->primitives_count,
                "Morph animation has no mesh");
        result.width = node->mesh->primitives[0].targets_count;
      } else
        result.width =
            channel.target_path == cgltf_animation_path_type_rotation ? 4 : 3;
      require(result.width > 0 && result.width <= 64,
              "Invalid animation sample width");
      auto sampler = channel.sampler;
      size_t components = cgltf_num_components(sampler->output->type);
      require(components > 0 && sampler->output->count <=
                                    (8000000 - animationValues) / components,
              "Animation data exceeds preview limits");
      animationValues += sampler->output->count * components;
      charge(sampler->output->count * components * sizeof(float));
      result.values =
          unpack(sampler->output, components, sampler->output->count);
      size_t multiplier =
          sampler->interpolation == cgltf_interpolation_type_cubic_spline ? 3
                                                                          : 1;
      require(result.values.size() == count * result.width * multiplier,
              "Invalid glTF animation output count");
      out.channels.push_back(std::move(result));
    }
    model->animations.push_back(std::move(out));
  }
  model->dependencies = std::move(budget.dependencies);
  std::sort(model->dependencies.begin(), model->dependencies.end());
  model->dependencies.erase(
      std::unique(model->dependencies.begin(), model->dependencies.end()),
      model->dependencies.end());
  auto frame = model->evaluate(0, -1);
  for (auto &draw : frame.draws) {
    auto &vertices = draw.deformed ? draw.vertices : draw.primitive->vertices;
    for (auto &v : vertices)
      model->bounds.include(point(draw.world, v.position));
  }
  require(model->bounds.valid, "glTF scene has no renderable geometry");
  return model;
}
Frame Model::evaluate(double seconds, int animation) const {
  require(std::isfinite(seconds), "Animation time must be finite");
  auto frameNodes = nodes;
  if (animation >= 0 && animation < int(animations.size())) {
    auto &clip = animations[animation];
    float time =
        clip.duration > 0 ? std::fmod(std::max(0., seconds), clip.duration) : 0;
    for (auto &c : clip.channels) {
      auto upper = std::upper_bound(c.times.begin(), c.times.end(), time);
      size_t b = std::min(size_t(upper - c.times.begin()), c.times.size() - 1),
             a = b ? b - 1 : 0;
      if (time >= c.times.back())
        a = b = c.times.size() - 1;
      float span = c.times[b] - c.times[a],
            t = span > 0 ? std::clamp((time - c.times[a]) / span, 0.f, 1.f) : 0;
      std::vector<float> value(c.width);
      bool cubic = c.interpolation == cgltf_interpolation_type_cubic_spline;
      for (int k = 0; k < c.width; ++k) {
        if (cubic) {
          float t2 = t * t, t3 = t2 * t;
          value[k] =
              (2 * t3 - 3 * t2 + 1) * c.values[(a * 3 + 1) * c.width + k] +
              (t3 - 2 * t2 + t) * span * c.values[(a * 3 + 2) * c.width + k] +
              (-2 * t3 + 3 * t2) * c.values[(b * 3 + 1) * c.width + k] +
              (t3 - t2) * span * c.values[(b * 3) * c.width + k];
        } else {
          float x = c.values[a * c.width + k], y = c.values[b * c.width + k];
          value[k] = c.interpolation == cgltf_interpolation_type_step
                         ? x
                         : x + (y - x) * t;
        }
      }
      for (float v : value)
        require(std::isfinite(v), "glTF animation overflow");
      auto &node = frameNodes[c.node];
      if (c.path == cgltf_animation_path_type_translation)
        node.translation = vec(value.data());
      else if (c.path == cgltf_animation_path_type_scale)
        node.scale = vec(value.data());
      else if (c.path == cgltf_animation_path_type_rotation) {
        if (!cubic && c.interpolation != cgltf_interpolation_type_step) {
          auto x = &c.values[a * 4], y = &c.values[b * 4];
          node.rotation =
              slerp({x[0], x[1], x[2], x[3]}, {y[0], y[1], y[2], y[3]}, t);
        } else
          node.rotation = normalized({value[0], value[1], value[2], value[3]});
      } else if (c.path == cgltf_animation_path_type_weights)
        node.weights = value;
    }
  }
  std::vector<Mat4> world(nodes.size());
  std::vector<unsigned char> visited(nodes.size());
  std::function<void(int, int)> resolve = [&](int i, int depth) {
    require(depth < 256, "glTF hierarchy exceeds 255 levels");
    if (visited[i] == 2)
      return;
    require(visited[i] != 1, "Cyclic glTF hierarchy");
    visited[i] = 1;
    auto &n = frameNodes[i];
    auto local =
        n.hasMatrix ? n.matrix : trs(n.translation, n.rotation, n.scale);
    if (n.parent >= 0) {
      resolve(n.parent, depth + 1);
      local = world[n.parent] * local;
    }
    for (float value : local.v)
      require(std::isfinite(value), "glTF transform overflow");
    world[i] = local;
    visited[i] = 2;
  };
  for (size_t i = 0; i < nodes.size(); ++i)
    resolve(i, 0);
  Frame frame;
  size_t deformedVertices = 0;
  for (size_t ni = 0; ni < nodes.size(); ++ni) {
    auto &node = frameNodes[ni];
    if (!node.active || node.mesh < 0)
      continue;
    auto &mesh = meshes[node.mesh];
    std::vector<Mat4> joints;
    if (node.skin >= 0) {
      auto &skin = skins[node.skin];
      Mat4 inv;
      try {
        inv = inverse(world[ni]);
      } catch (...) {
        continue;
      }
      for (size_t j = 0; j < skin.joints.size(); ++j)
        joints.push_back(inv * world[skin.joints[j]] * skin.inverseBind[j]);
    }
    for (size_t pi = 0; pi < mesh.primitives.size(); ++pi) {
      auto &primitive = mesh.primitives[pi];
      Draw draw;
      draw.primitive = &primitive;
      draw.world = world[ni];
      draw.node = ni;
      draw.primitiveIndex = pi;
      draw.deformed = !joints.empty() && !primitive.influences.empty() ||
                      !primitive.morphs.empty();
      if (draw.deformed) {
        require(primitive.vertices.size() <= vertexLimit - deformedVertices,
                "Animated scene exceeds two million "
                "deformed vertices");
        deformedVertices += primitive.vertices.size();
        draw.vertices = primitive.vertices;
        for (size_t v = 0; v < draw.vertices.size(); ++v) {
          auto &vertex = draw.vertices[v];
          for (size_t m = 0; m < primitive.morphs.size(); ++m) {
            float weight = m < node.weights.size() ? node.weights[m] : 0;
            if (weight) {
              auto &morph = primitive.morphs[m];
              if (!morph.positions.empty())
                vertex.position = vertex.position + morph.positions[v] * weight;
              if (!morph.normals.empty())
                vertex.normal = vertex.normal + morph.normals[v] * weight;
            }
          }
          if (!joints.empty() && !primitive.influences.empty()) {
            auto &influence = primitive.influences[v];
            Vec3 position{}, normal{};
            float sum = 0;
            for (int k = 0; k < 8; ++k)
              if (influence.weights[k] > 0) {
                require(influence.joints[k] < joints.size(), "Skin "
                                                             "joint "
                                                             "index is "
                                                             "outside "
                                                             "skeleton");
                auto &matrix = joints[influence.joints[k]];
                position = position + point(matrix, vertex.position) *
                                          influence.weights[k];
                normal = normal +
                         vector(matrix, vertex.normal) * influence.weights[k];
                sum += influence.weights[k];
              }
            if (sum > 0) {
              vertex.position = position / sum;
              vertex.normal = normalized(normal, {0, 1, 0});
            }
          }
          require(finite(vertex.position) && finite(vertex.normal),
                  "glTF deformation overflow");
        }
      }
      require(frame.draws.size() < 16384,
              "glTF scene exceeds 16384 draw instances");
      frame.draws.push_back(std::move(draw));
    }
  }
  return frame;
}
Bounds Model::normalizedBounds() const {
  Bounds result;
  result.include(point(bounds.fit(), bounds.min));
  result.include(point(bounds.fit(), bounds.max));
  return result;
}
size_t Model::triangleCount() const {
  size_t result = 0;
  for (auto &mesh : meshes)
    for (auto &p : mesh.primitives)
      if (p.mode == 4)
        result += p.indices.size() / 3;
  return result;
}
} // namespace fsv::gltf
