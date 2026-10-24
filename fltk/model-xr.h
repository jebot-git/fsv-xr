// SPDX-License-Identifier: MIT
#pragma once
#include <string>

#include "gltf-model.h"
namespace fsv::gltf {
struct XrProbe {
  bool available = false;
  std::string runtime, message;
};
XrProbe probeXr();
struct Binding {
  std::string action, path;
};
struct Profile {
  std::string path, extension;
  bool touch = false;
};
std::vector<Profile>
controllerProfiles(const std::vector<std::string> &extensions);
std::vector<Binding> controllerBindings(const Profile &);
// Overlay by default: never replace DomeD's primary session implicitly.
int runXr(const std::filesystem::path &, double seconds = 0,
          const std::filesystem::path &report = {}, bool exclusive = false);
void renderModel(const std::filesystem::path &,
                 const std::filesystem::path &ppm, double seconds = 0,
                 int animation = 0);
} // namespace fsv::gltf
