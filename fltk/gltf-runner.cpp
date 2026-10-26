// SPDX-License-Identifier: MIT
#include <chrono>
#include <iostream>
#include <nlohmann/json.hpp>

#include "gltf-render.h"
#include "model-egl.h"
#include "model-xr.h"
int main(int argc, char **argv) {
  try {
    std::string mode = argc > 1 ? argv[1] : "probe";
    if (mode == "probe") {
      auto p = fsv::gltf::probeXr();
      std::cout << nlohmann::json{{"available", p.available},
                                  {"runtime", p.runtime},
                                  {"message", p.message}}
                       .dump()
                << '\n';
      return 0;
    }
    if (mode == "render" && argc >= 4) {
      fsv::gltf::renderModel(argv[2], argv[3],
                             argc > 4 ? std::stod(argv[4]) : 0);
      return 0;
    }
    if (mode == "cache" && argc >= 3) {
      auto m = fsv::gltf::load(argv[2]);
      fsv::gltf::EglContext context;
      fsv::gltf::Renderer renderer;
      renderer.prepare(m, 0);
      auto uploads = renderer.uploadedVertices();
      auto began = std::chrono::steady_clock::now();
      for (int i = 0; i < 1000; ++i)
        renderer.prepare(m, i * .01);
      std::cout << nlohmann::json{{"initial_uploads", uploads},
                                  {"final_uploads",
                                   renderer.uploadedVertices()},
                                  {"seconds",
                                   std::chrono::duration<double>(
                                       std::chrono::steady_clock::now() - began)
                                       .count()}}
                       .dump()
                << '\n';
      return 0;
    }
    if (mode == "inspect" && argc >= 3) {
      auto m = fsv::gltf::load(argv[2]);
      auto frame = m->evaluate(argc > 3 ? std::stod(argv[3]) : 0);
      nlohmann::json draws = nlohmann::json::array();
      for (auto &d : frame.draws) {
        nlohmann::json positions = nlohmann::json::array();
        for (auto &v : d.deformed ? d.vertices : d.primitive->vertices) {
          auto p = fsv::gltf::point(d.world, v.position);
          positions.push_back({p.x, p.y, p.z});
        }
        draws.push_back(positions);
      }
      std::cout << nlohmann::json{{"draws", draws},
                                  {"images", m->images.size()},
                                  {"triangles", m->triangleCount()}}
                       .dump()
                << '\n';
      return 0;
    }
    if (mode == "xr" && argc >= 3)
      return fsv::gltf::runXr(argv[2], argc > 3 ? std::stod(argv[3]) : 0,
                              argc > 4 ? argv[4] : "");
    std::cerr << "Usage: fsv-gltf-runner probe|render FILE OUT.ppm "
                 "[TIME]|xr FILE [SECONDS] [REPORT]\n";
    return 2;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
