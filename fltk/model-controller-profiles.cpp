// SPDX-License-Identifier: MIT
#include <algorithm>

#include "model-xr.h"
namespace fsv::gltf {
std::vector<Profile>
controllerProfiles(const std::vector<std::string> &extensions) {
  std::vector<Profile> out{
      {"/interaction_profiles/oculus/touch_controller", "", true}};
  for (auto profile :
       {Profile{"/interaction_profiles/facebook/touch_controller_pro",
                "XR_FB_touch_controller_pro", true},
        Profile{"/interaction_profiles/meta/touch_controller_plus",
                "XR_META_touch_controller_plus", true}})
    if (std::find(extensions.begin(), extensions.end(), profile.extension) !=
        extensions.end())
      out.push_back(profile);
  for (auto path : {"/interaction_profiles/valve/index_controller",
                    "/interaction_profiles/htc/vive_controller",
                    "/interaction_profiles/khr/simple_controller"})
    out.push_back({path, "", false});
  return out;
}
std::vector<Binding> controllerBindings(const Profile &profile) {
  std::vector<Binding> out;
  for (auto hand : {"left", "right"}) {
    std::string root = "/user/hand/" + std::string(hand) + "/";
    out.push_back({"grip", root + "input/grip/pose"});
    out.push_back({"aim", root + "input/aim/pose"});
    out.push_back({"haptic", root + "output/haptic"});
    out.push_back(
        {"squeeze", root + (profile.path.find("simple") != std::string::npos
                                ? "input/select/click"
                            : profile.path.find("vive") != std::string::npos
                                ? "input/squeeze/click"
                                : "input/squeeze/value")});
  }
  if (profile.touch) {
    out.push_back({"reset", "/user/hand/left/input/x/click"});
    out.push_back({"close", "/user/hand/right/input/b/click"});
  } else if (profile.path.find("index") != std::string::npos) {
    out.push_back({"reset", "/user/hand/left/input/a/click"});
    out.push_back({"close", "/user/hand/right/input/b/click"});
  } else if (profile.path.find("vive") != std::string::npos) {
    out.push_back({"reset", "/user/hand/left/input/menu/click"});
    out.push_back({"close", "/user/hand/right/input/menu/click"});
  } else
    out.push_back({"close", "/user/hand/right/input/menu/click"});
  return out;
}
} // namespace fsv::gltf
