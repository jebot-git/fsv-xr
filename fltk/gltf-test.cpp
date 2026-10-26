// SPDX-License-Identifier: MIT
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>

#include "gltf-model.h"
#include "model-loader.h"
#include "object-interaction.h"
using namespace fsv::gltf;
using Json = nlohmann::json;
static bool close(float a, float b) { return std::abs(a - b) < 1e-4; }
static bool close(Vec3 a, Vec3 b) { return length(a - b) < 1e-4; }
int main() {
  char temp[] = "/tmp/fsvr-gltf-test-XXXXXX";
  auto root = std::filesystem::path(mkdtemp(temp));
  try {
    std::vector<unsigned char> data;
    auto bytes = [&](const auto &v) {
      while (data.size() % 4)
        data.push_back(0);
      size_t offset = data.size();
      auto p = reinterpret_cast<const unsigned char *>(v.data());
      data.insert(data.end(), p, p + v.size() * sizeof(v[0]));
      return offset;
    };
    std::vector<float> positions = {-.5f, -.5f, 0, .5f, -.5f, 0, 0, .5f, 0};
    auto position = bytes(positions);
    auto times = bytes(std::vector<float>{0, 1});
    auto translation = bytes(std::vector<float>{2, 0, 0, 4, 0, 0});
    Json asset = {
        {"asset", {{"version", "2.0"}}},
        {"buffers",
         Json::array({{{"uri", "triangle.bin"}, {"byteLength", data.size()}}})},
        {"bufferViews",
         Json::array(
             {{{"buffer", 0}, {"byteOffset", position}, {"byteLength", 36}},
              {{"buffer", 0}, {"byteOffset", times}, {"byteLength", 8}},
              {{"buffer", 0},
               {"byteOffset", translation},
               {"byteLength", 24}}})},
        {"accessors", Json::array({{{"bufferView", 0},
                                    {"componentType", 5126},
                                    {"count", 3},
                                    {"type", "VEC3"},
                                    {"min", {-.5, -.5, 0}},
                                    {"max", {.5, .5, 0}}},
                                   {{"bufferView", 1},
                                    {"componentType", 5126},
                                    {"count", 2},
                                    {"type", "SCALAR"}},
                                   {{"bufferView", 2},
                                    {"componentType", 5126},
                                    {"count", 2},
                                    {"type", "VEC3"}}})},
        {"materials", Json::array({{{"pbrMetallicRoughness",
                                     {{"baseColorFactor", {.9, .1, .1, 1}},
                                      {"metallicFactor", 0}}}}})},
        {"meshes",
         Json::array(
             {{{"primitives", Json::array({{{"attributes", {{"POSITION", 0}}},
                                            {"material", 0}}})}}})},
        {"nodes", Json::array({{{"mesh", 0}, {"translation", {2, 0, 0}}}})},
        {"scenes", Json::array({{{"nodes", {0}}}})},
        {"scene", 0},
        {"animations",
         Json::array(
             {{{"samplers", Json::array({{{"input", 1},
                                          {"output", 2},
                                          {"interpolation", "LINEAR"}}})},
               {"channels",
                Json::array({{{"sampler", 0},
                              {"target",
                               {{"node", 0}, {"path", "translation"}}}}})}}})}};
    {
      std::ofstream out(root / "triangle.bin", std::ios::binary);
      out.write(reinterpret_cast<char *>(data.data()), data.size());
    }
    auto save = [&](const Json &j) {
      std::ofstream(root / "triangle.gltf") << j.dump();
    };
    save(asset);
    auto model = load(root / "triangle.gltf");
    assert(model->triangleCount() == 1);
    assert(close(model->bounds.min, {1.5f, -.5f, 0}));
    assert(model->dependencies.size() == 2);
    auto frame = model->evaluate(.5);
    assert(frame.draws.size() == 1);
    assert(close(point(frame.draws[0].world, {0, 0, 0}), {3, 0, 0}));
    auto bounds = model->normalizedBounds();
    assert(close(bounds.min, {-.5, -.5, 0}) && close(bounds.max, {.5, .5, 0}));
    auto invalid = asset;
    invalid["bufferViews"][0]["byteLength"] = 1;
    save(invalid);
    bool rejected = false;
    try {
      load(root / "triangle.gltf");
    } catch (...) {
      rejected = true;
    }
    assert(rejected);
    invalid = asset;
    invalid["extensionsRequired"] = {"KHR_draco_mesh_compression"};
    save(invalid);
    rejected = false;
    try {
      load(root / "triangle.gltf");
    } catch (...) {
      rejected = true;
    }
    assert(rejected);
    save(asset);
    {
      Loader loader;
      auto wait = [&](auto predicate) {
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!predicate()) {
          assert(std::chrono::steady_clock::now() < until);
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
      };
      loader.open(root / "triangle.gltf");
      wait([&] { return bool(loader.snapshot().model); });
      auto usable = loader.snapshot().model;
      std::ofstream(root / "triangle.gltf") << "{ broken edit";
      wait([&] { return !loader.snapshot().error.empty(); });
      assert(loader.snapshot().model == usable);
      save(asset);
      wait([&] {
        return loader.snapshot().model != usable &&
               loader.snapshot().error.empty();
      });
      std::ofstream(root / "latest.gltf") << asset.dump();
      for (int i = 0; i < 100; ++i)
        loader.open(root / (i % 2 ? "missing.gltf" : "triangle.gltf"));
      loader.open(root / "latest.gltf");
      wait([&] {
        auto result = loader.snapshot();
        return result.model && result.model->path == root / "latest.gltf" &&
               !result.loading;
      });
      loader.clear();
      wait([&] {
        return !loader.snapshot().model && !loader.snapshot().loading;
      });
    }
    auto began = std::chrono::steady_clock::now();
    {
      Loader loader;
      loader.open(root / "triangle.gltf");
    }
    assert(std::chrono::steady_clock::now() - began <
           std::chrono::milliseconds(100));
    auto cancelled = std::make_shared<std::atomic_bool>(true);
    rejected = false;
    try {
      load(root / "triangle.gltf", cancelled);
    } catch (...) {
      rejected = true;
    }
    assert(rejected);
    std::cout << "PASS background reload, last usable object, "
                 "cancellation, coalescing and nonblocking destruction\n";
    // Binary glTF follows the same rendering path as
    // external-buffer JSON glTF.
    auto glb = asset;
    glb["buffers"][0].erase("uri");
    std::string json = glb.dump();
    while (json.size() % 4)
      json += ' ';
    while (data.size() % 4)
      data.push_back(0);
    {
      std::ofstream out(root / "triangle.glb", std::ios::binary);
      auto u32 = [&](uint32_t v) {
        out.write(reinterpret_cast<char *>(&v), 4);
      };
      u32(0x46546c67);
      u32(2);
      u32(12 + 8 + json.size() + 8 + data.size());
      u32(json.size());
      u32(0x4e4f534a);
      out.write(json.data(), json.size());
      u32(data.size());
      u32(0x004e4942);
      out.write(reinterpret_cast<char *>(data.data()), data.size());
    }
    assert(load(root / "triangle.glb")->triangleCount() == 1);
    std::cout << "PASS glTF/GLB geometry, materials, animation, "
                 "transforms, dependency watches and malformed data\n";
    Bounds box;
    box.include({-.5, -.5, -.5});
    box.include({.5, .5, .5});
    Manipulator grab;
    grab.object = {{0, 0, -1}, {}, 1};
    std::array<Hand, 2> hands;
    hands[0] = {{{-.25, 0, -1}, {}, 1}, {}, true, false, 1};
    grab.update(hands, box);
    assert(grab.grabbing() == 1);
    hands[0].grip.position.x += .4;
    grab.update(hands, box);
    assert(close(grab.object.position, {.4, 0, -1}));
    auto before = grab.object;
    hands[0].valid = false;
    grab.update(hands, box);
    assert(!grab.grabbing() && close(grab.object.position, before.position));
    hands[0].valid = true;
    grab.update(hands, box);
    assert(!grab.grabbing());
    hands[0].squeeze = 0;
    grab.update(hands, box);
    grab.object = {{0, 0, -1}, {}, 1};
    hands = {};
    hands[0] = {{{-.25, 0, -1}, {}, 1}, {}, true, false, 1};
    hands[1] = {{{.25, 0, -1}, {}, 1}, {}, true, false, 1};
    grab.update(hands, box);
    assert(grab.grabbing() == 3);
    hands[0].grip.position.x = -.5;
    hands[1].grip.position.x = .5;
    grab.update(hands, box);
    assert(close(grab.object.scale, 2));
    const float q = std::sqrt(.5f);
    hands[0].grip.position = {0, -.5f, -1};
    hands[1].grip.position = {0, .5f, -1};
    hands[0].grip.orientation = hands[1].grip.orientation = {0, 0, q, q};
    grab.update(hands, box);
    assert(close(rotate(grab.object.orientation, {1, 0, 0}), {0, 1, 0}));
    before = grab.object;
    hands[1].squeeze = 0;
    grab.update(hands, box);
    assert(grab.grabbing() == 1 &&
           close(grab.object.position, before.position));
    hands[0].grip.position.z -= .2;
    grab.update(hands, box);
    assert(close(grab.object.position.z, before.position.z - .2f));
    grab.update(hands, box, false);
    assert(!grab.grabbing());
    // Distant ray acquisition, bounded scaling and
    // coincident-controller safety.
    Manipulator ray;
    ray.object = {{0, 0, -2}, {}, 1};
    hands = {};
    hands[0] = {{{2, 0, 0}, {}, 1}, {{0, 0, 0}, {}, 1}, true, true, 1};
    ray.update(hands, box);
    assert(ray.grabbing() == 1);
    hands[0].grip.position.y += .25;
    ray.update(hands, box);
    assert(close(ray.object.position.y, .25));
    Manipulator extreme;
    extreme.object = {{0, 0, -1}, {}, 1};
    hands = {};
    hands[0] = {{{-.1, 0, -1}, {}, 1}, {}, true, false, 1};
    hands[1] = {{{.1, 0, -1}, {}, 1}, {}, true, false, 1};
    extreme.update(hands, box);
    hands[0].grip.position.x = -100;
    hands[1].grip.position.x = 100;
    extreme.update(hands, box);
    assert(close(extreme.object.scale, 20));
    before = extreme.object;
    hands[0].grip.position = hands[1].grip.position;
    extreme.update(hands, box);
    assert(close(extreme.object.scale, before.scale));
    hands[0].grip.position = {-.002, 0, -1};
    hands[1].grip.position = {.002, 0, -1};
    extreme.update(hands, box);
    assert(finite(extreme.object));
    auto identity = inverse(trs({1, 2, 3}, {0, 0, q, q}, {2, 3, 4})) *
                    trs({1, 2, 3}, {0, 0, q, q}, {2, 3, 4});
    assert(close(point(identity, {.4f, .8f, 1.2f}), {.4f, .8f, 1.2f}));
    std::cout << "PASS one/two-controller grab, move, rotation, scale, "
                 "tracking/focus loss and release rebasing\n";
  } catch (...) {
    std::filesystem::remove_all(root);
    throw;
  }
  std::filesystem::remove_all(root);
}
