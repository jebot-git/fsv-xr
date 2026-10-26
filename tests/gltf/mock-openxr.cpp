// SPDX-License-Identifier: MIT
// Isolated API-level runtime for controller-to-stereo-frame regression tests.
// It never connects to the workstation's OpenXR service or input devices.
#define XR_USE_PLATFORM_EGL
#define XR_USE_GRAPHICS_API_OPENGL
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <GL/gl.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>
namespace {
struct Action {
  std::string name;
  XrActionType type;
};
struct Space {
  std::string action;
  int hand = -1;
};
struct Chain {
  GLuint texture = 0;
  int width = 0, height = 0;
};
unsigned frame = 0;
int pollStage = 0;
bool lost = false, restored = false;
uint64_t nextPath = 1;
std::map<std::string, XrPath> paths;
std::map<XrPath, std::string> names;
std::vector<std::string> profiles;
bool overlay = false;
int pulses = 0, stereoFrames = 0;
XrPath path(const std::string &s) {
  auto i = paths.find(s);
  if (i != paths.end())
    return i->second;
  auto id = nextPath++;
  paths[s] = id;
  names[id] = s;
  return id;
}
int hand(XrPath p) {
  return names[p].find("left") != std::string::npos ? 0 : 1;
}
XrPosef tracked(int h) {
  XrPosef p{{0, 0, 0, 1}, {0, 1.6f, 0}};
  if (h < 0)
    return p;
  float centerX = .1f, centerY = 1.68f, z = -1.2f;
  p.position = {h ? .2f : -.2f, 1.48f, z};
  if (frame >= 5 && frame <= 15 && h == 0)
    p.position.x = -.2f + (frame - 5) * .03f;
  else if (frame > 15 && frame < 35 && h == 0) {
    p.position.x = .1f;
    if (frame >= 20) {
      float a = std::min(1.f, (frame - 20) / 10.f) * 1.5707963268f;
      p.orientation = {0, 0, std::sin(a / 2), std::cos(a / 2)};
    }
  } else if (frame >= 35) {
    float span =
        frame < 45 ? .15f + std::max(0.f, (frame - 35) / 10.f) * .15f : .3f;
    float a =
        frame < 45 ? 0 : std::min(1.f, (frame - 45) / 10.f) * 1.5707963268f;
    float sign = h ? 1.f : -1.f;
    p.position = {centerX + sign * span * std::cos(a), centerY,
                  z - sign * span * std::sin(a)};
    p.orientation = {0, std::sin(a / 2), 0, std::cos(a / 2)};
    if (frame >= 56 && h == 1)
      p.position.z -= std::min(1.f, (frame - 56) / 4.f) * .2f;
  }
  return p;
}
float squeeze(int h) {
  if (frame >= 5 && frame <= 15)
    return h == 0 ? 1 : 0;
  if (frame >= 20 && frame <= 30)
    return h == 0 ? 1 : 0;
  if (frame >= 35 && frame < 56)
    return 1;
  if (frame >= 56 && frame < 70)
    return h == 1 ? 1 : 0;
  return 0;
}
XrResult requirements(XrInstance, XrSystemId,
                      XrGraphicsRequirementsOpenGLKHR *out) {
  out->minApiVersionSupported = XR_MAKE_VERSION(3, 3, 0);
  out->maxApiVersionSupported = XR_MAKE_VERSION(4, 6, 0);
  return XR_SUCCESS;
}
void state(XrEventDataBuffer *out, XrSessionState state) {
  auto e = reinterpret_cast<XrEventDataSessionStateChanged *>(out);
  *e = {XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED};
  e->session = reinterpret_cast<XrSession>(2);
  e->state = state;
}
} // namespace
__attribute__((destructor)) static void reportMock() {
  if (const char *file = std::getenv("FSVR_MOCK_REPORT"); file && stereoFrames)
    std::ofstream(file) << "{\"stereo_frames\":" << stereoFrames
                        << ",\"haptics\":" << pulses << "}\n";
}
extern "C" {
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateInstanceExtensionProperties(
    const char *, uint32_t capacity, uint32_t *count,
    XrExtensionProperties *out) {
  const char *values[] = {XR_KHR_OPENGL_ENABLE_EXTENSION_NAME,
                          XR_MNDX_EGL_ENABLE_EXTENSION_NAME,
                          XR_EXTX_OVERLAY_EXTENSION_NAME};
  *count = 3;
  if (capacity) {
    if (capacity < 3)
      return XR_ERROR_SIZE_INSUFFICIENT;
    for (int i = 0; i < 3; ++i) {
      out[i] = {XR_TYPE_EXTENSION_PROPERTIES};
      strcpy(out[i].extensionName, values[i]);
      out[i].extensionVersion = 1;
    }
  }
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateInstance(const XrInstanceCreateInfo *,
                                                XrInstance *out) {
  *out = reinterpret_cast<XrInstance>(1);
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroyInstance(XrInstance) {
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL
xrGetInstanceProperties(XrInstance, XrInstanceProperties *out) {
  strcpy(out->runtimeName, "FSVR isolated controller test");
  out->runtimeVersion = 1;
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance,
                                                     const char *name,
                                                     PFN_xrVoidFunction *out) {
  if (!strcmp(name, "xrGetOpenGLGraphicsRequirementsKHR")) {
    *out = reinterpret_cast<PFN_xrVoidFunction>(requirements);
    return XR_SUCCESS;
  }
  *out = nullptr;
  return XR_ERROR_FUNCTION_UNSUPPORTED;
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetSystem(XrInstance, const XrSystemGetInfo *,
                                           XrSystemId *out) {
  *out = 1;
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateSession(XrInstance,
                                               const XrSessionCreateInfo *info,
                                               XrSession *out) {
  auto binding = static_cast<const XrGraphicsBindingEGLMNDX *>(info->next);
  if (!binding || binding->type != XR_TYPE_GRAPHICS_BINDING_EGL_MNDX)
    return XR_ERROR_GRAPHICS_DEVICE_INVALID;
  auto layer =
      static_cast<const XrSessionCreateInfoOverlayEXTX *>(binding->next);
  overlay = layer && layer->type == XR_TYPE_SESSION_CREATE_INFO_OVERLAY_EXTX;
  if (!overlay)
    return XR_ERROR_VALIDATION_FAILURE;
  *out = reinterpret_cast<XrSession>(2);
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroySession(XrSession) {
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateReferenceSpace(
    XrSession, const XrReferenceSpaceCreateInfo *, XrSpace *out) {
  *out = reinterpret_cast<XrSpace>(new Space{"head", -1});
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroySpace(XrSpace space) {
  delete reinterpret_cast<Space *>(space);
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSet(XrInstance,
                                                 const XrActionSetCreateInfo *,
                                                 XrActionSet *out) {
  *out = reinterpret_cast<XrActionSet>(3);
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroyActionSet(XrActionSet) {
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateAction(XrActionSet,
                                              const XrActionCreateInfo *info,
                                              XrAction *out) {
  *out = reinterpret_cast<XrAction>(
      new Action{info->actionName, info->actionType});
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrStringToPath(XrInstance, const char *value,
                                              XrPath *out) {
  *out = path(value);
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrSuggestInteractionProfileBindings(
    XrInstance, const XrInteractionProfileSuggestedBinding *info) {
  auto profile = names[info->interactionProfile];
  profiles.push_back(profile);
  if (profiles.size() == 1 &&
      profile != "/interaction_profiles/oculus/touch_controller")
    return XR_ERROR_VALIDATION_FAILURE;
  if (profile == "/interaction_profiles/oculus/touch_controller") {
    bool left = false, right = false, reset = false, close = false;
    for (uint32_t i = 0; i < info->countSuggestedBindings; ++i) {
      auto &b = info->suggestedBindings[i];
      auto a = reinterpret_cast<Action *>(b.action);
      auto &n = names[b.binding];
      left |=
          a->name == "squeeze" && n == "/user/hand/left/input/squeeze/value";
      right |=
          a->name == "squeeze" && n == "/user/hand/right/input/squeeze/value";
      reset |= a->name == "reset" && n == "/user/hand/left/input/x/click";
      close |= a->name == "close" && n == "/user/hand/right/input/b/click";
    }
    if (!left || !right || !reset || !close)
      return XR_ERROR_VALIDATION_FAILURE;
  }
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL
xrAttachSessionActionSets(XrSession, const XrSessionActionSetsAttachInfo *) {
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSpace(
    XrSession, const XrActionSpaceCreateInfo *info, XrSpace *out) {
  *out = reinterpret_cast<XrSpace>(
      new Space{reinterpret_cast<Action *>(info->action)->name,
                hand(info->subactionPath)});
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateEnvironmentBlendModes(
    XrInstance, XrSystemId, XrViewConfigurationType, uint32_t capacity,
    uint32_t *count, XrEnvironmentBlendMode *out) {
  *count = 1;
  if (capacity)
    out[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateViewConfigurationViews(
    XrInstance, XrSystemId, XrViewConfigurationType, uint32_t capacity,
    uint32_t *count, XrViewConfigurationView *out) {
  *count = 2;
  if (capacity < 2)
    return capacity ? XR_ERROR_SIZE_INSUFFICIENT : XR_SUCCESS;
  for (int i = 0; i < 2; ++i) {
    out[i] = {XR_TYPE_VIEW_CONFIGURATION_VIEW};
    out[i].recommendedImageRectWidth = out[i].recommendedImageRectHeight = 128;
    out[i].maxImageRectWidth = out[i].maxImageRectHeight = 128;
    out[i].recommendedSwapchainSampleCount = out[i].maxSwapchainSampleCount = 1;
  }
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainFormats(XrSession,
                                                           uint32_t capacity,
                                                           uint32_t *count,
                                                           int64_t *out) {
  *count = 1;
  if (capacity)
    out[0] = GL_SRGB8_ALPHA8;
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateSwapchain(
    XrSession, const XrSwapchainCreateInfo *info, XrSwapchain *out) {
  auto c = new Chain;
  c->width = info->width;
  c->height = info->height;
  glGenTextures(1, &c->texture);
  glBindTexture(GL_TEXTURE_2D, c->texture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, c->width, c->height, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  *out = reinterpret_cast<XrSwapchain>(c);
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroySwapchain(XrSwapchain value) {
  auto c = reinterpret_cast<Chain *>(value);
  glDeleteTextures(1, &c->texture);
  delete c;
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL
xrEnumerateSwapchainImages(XrSwapchain value, uint32_t capacity,
                           uint32_t *count, XrSwapchainImageBaseHeader *out) {
  *count = 1;
  if (capacity)
    reinterpret_cast<XrSwapchainImageOpenGLKHR *>(out)->image =
        reinterpret_cast<Chain *>(value)->texture;
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrPollEvent(XrInstance, XrEventDataBuffer *out) {
  if (pollStage == 0) {
    ++pollStage;
    state(out, XR_SESSION_STATE_READY);
    return XR_SUCCESS;
  }
  if (pollStage == 1) {
    ++pollStage;
    state(out, XR_SESSION_STATE_FOCUSED);
    return XR_SUCCESS;
  }
  if (frame >= 64 && !lost) {
    lost = true;
    state(out, XR_SESSION_STATE_VISIBLE);
    return XR_SUCCESS;
  }
  if (frame >= 67 && !restored) {
    restored = true;
    state(out, XR_SESSION_STATE_FOCUSED);
    return XR_SUCCESS;
  }
  return XR_EVENT_UNAVAILABLE;
}
XRAPI_ATTR XrResult XRAPI_CALL xrBeginSession(XrSession,
                                              const XrSessionBeginInfo *) {
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrEndSession(XrSession) { return XR_SUCCESS; }
XRAPI_ATTR XrResult XRAPI_CALL xrWaitFrame(XrSession, const XrFrameWaitInfo *,
                                           XrFrameState *out) {
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ++frame;
  out->predictedDisplayTime = frame * 11111111LL;
  out->predictedDisplayPeriod = 11111111;
  out->shouldRender = XR_TRUE;
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrBeginFrame(XrSession,
                                            const XrFrameBeginInfo *) {
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrSyncActions(XrSession,
                                             const XrActionsSyncInfo *) {
  return frame >= 65 && frame <= 67 ? XR_SESSION_NOT_FOCUSED : XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStatePose(
    XrSession, const XrActionStateGetInfo *, XrActionStatePose *out) {
  out->isActive = !(frame >= 65 && frame <= 67);
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateFloat(
    XrSession, const XrActionStateGetInfo *info, XrActionStateFloat *out) {
  out->isActive = XR_TRUE;
  out->currentState = squeeze(hand(info->subactionPath));
  out->changedSinceLastSync = XR_TRUE;
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateBoolean(
    XrSession, const XrActionStateGetInfo *info, XrActionStateBoolean *out) {
  auto a = reinterpret_cast<Action *>(info->action);
  out->isActive = XR_TRUE;
  out->changedSinceLastSync = XR_TRUE;
  out->currentState =
      a->name == "close" && frame >= 85 && hand(info->subactionPath) == 1;
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrLocateSpace(XrSpace value, XrSpace, XrTime,
                                             XrSpaceLocation *out) {
  auto s = reinterpret_cast<Space *>(value);
  out->locationFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT |
                       XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                       XR_SPACE_LOCATION_POSITION_TRACKED_BIT |
                       XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
  out->pose = tracked(s->hand);
  if (s->hand >= 0 && frame >= 65 && frame <= 67)
    out->locationFlags = 0;
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrLocateViews(XrSession,
                                             const XrViewLocateInfo *,
                                             XrViewState *out,
                                             uint32_t capacity, uint32_t *count,
                                             XrView *views) {
  *count = 2;
  if (capacity < 2)
    return XR_ERROR_SIZE_INSUFFICIENT;
  out->viewStateFlags =
      XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
  for (int i = 0; i < 2; ++i) {
    views[i].pose = tracked(-1);
    views[i].pose.position.x = i ? .032f : -.032f;
    views[i].fov = {-.65f, .65f, .65f, -.65f};
  }
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrAcquireSwapchainImage(
    XrSwapchain, const XrSwapchainImageAcquireInfo *, uint32_t *out) {
  *out = 0;
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL
xrWaitSwapchainImage(XrSwapchain, const XrSwapchainImageWaitInfo *) {
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL
xrReleaseSwapchainImage(XrSwapchain, const XrSwapchainImageReleaseInfo *) {
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrApplyHapticFeedback(
    XrSession, const XrHapticActionInfo *, const XrHapticBaseHeader *) {
  ++pulses;
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrEndFrame(XrSession,
                                          const XrFrameEndInfo *info) {
  if (info->layerCount) {
    auto layer =
        reinterpret_cast<const XrCompositionLayerProjection *>(info->layers[0]);
    if (layer->viewCount != 2 ||
        !(layer->layerFlags &
          XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT))
      return XR_ERROR_VALIDATION_FAILURE;
    ++stereoFrames;
  }
  return XR_SUCCESS;
}
}
