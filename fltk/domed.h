// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
namespace fsv_stereo {
// Validate and retain the launching Wayland realm; explicit choices only select
// a sibling socket owned by the same compositor. Returns desktop/xr/standalone.
std::string configureLaunchRealm(const std::string &requested = "inherit");
struct DomeMode {
  bool managed = false, known = false, xr = false;
  std::string message;
  float pulloutDistance = 2.f;
  float laneHeightRadians = .6f;
  bool hasPulloutAnchor = false;
  std::array<float, 3> pulloutPosition{};
  std::array<float, 4> pulloutOrientation{0, 0, 0, 1};
  uint64_t windowId = 0;
  bool stereoSupported = false, trackedEyes = false, hasWindowQuad = false;
  std::array<std::array<float, 3>, 2> eyes{
      {{-.032f, 1.6f, 0}, {.032f, 1.6f, 0}}};
  std::array<float, 3> headPosition{0, 1.6f, 0};
  std::array<float, 4> headOrientation{0, 0, 0, 1};
  std::array<float, 3> windowPosition{};
  std::array<float, 4> windowOrientation{0, 0, 0, 1};
  std::array<float, 2> windowSize{};
  bool hasCylinder = false;
  std::array<float, 3> cylinderPosition{};
  std::array<float, 4> cylinderOrientation{0, 0, 0, 1};
  float cylinderRadius = 2, cylinderHorizontal = 1, cylinderTop = .5f,
        cylinderBottom = -.5f;
  bool allowsXR() const { return !managed || (known && xr); }
};
// Observe only the compositor that owns this client's Wayland socket. All
// discovery and bounded loopback I/O run outside FLTK and the XR frame loop.
class DomeMonitor {
  struct State;
  std::shared_ptr<State> state;

public:
  DomeMonitor();
  ~DomeMonitor();
  DomeMode snapshot() const;
};
} // namespace fsv_stereo
