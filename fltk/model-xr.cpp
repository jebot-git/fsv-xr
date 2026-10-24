// SPDX-License-Identifier: MIT
#define GL_GLEXT_PROTOTYPES
#define XR_USE_PLATFORM_EGL
#define XR_USE_GRAPHICS_API_OPENGL
#include "model-xr.h"

#include <GL/gl.h>
#include <GL/glext.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>

#include "gltf-render.h"
#include "model-egl.h"
#include "model-loader.h"
#if FSVR_MODEL_XR
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#endif
namespace fsv::gltf {
using Json = nlohmann::json;
namespace {
void savePpm(const std::filesystem::path &path, int w, int h) {
  std::vector<unsigned char> pixels(size_t(w) * h * 3);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
  std::ofstream out(path, std::ios::binary);
  if (!out)
    throw std::runtime_error("Cannot create model capture");
  out << "P6\n" << w << ' ' << h << "\n255\n";
  for (int y = h - 1; y >= 0; --y)
    out.write(reinterpret_cast<char *>(pixels.data() + y * w * 3), w * 3);
  if (!out)
    throw std::runtime_error("Cannot write model capture");
}
#if FSVR_MODEL_XR
void check(XrResult result, const char *what) {
  if (XR_FAILED(result))
    throw std::runtime_error(std::string(what) + ": OpenXR error " +
                             std::to_string(result));
}
#define XR(call) check(call, #call)
std::vector<std::string> extensions() {
  uint32_t n = 0;
  XR(xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr));
  std::vector<XrExtensionProperties> values(n, {XR_TYPE_EXTENSION_PROPERTIES});
  XR(xrEnumerateInstanceExtensionProperties(nullptr, n, &n, values.data()));
  std::vector<std::string> out;
  for (auto &v : values)
    out.emplace_back(v.extensionName);
  return out;
}
bool has(const std::vector<std::string> &values, const char *name) {
  return std::find(values.begin(), values.end(), name) != values.end();
}
XrInstance instance(const std::vector<const char *> &extensions) {
  XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
  strcpy(info.applicationInfo.applicationName, "FSVR glTF object");
  strcpy(info.applicationInfo.engineName, "FSVR native OpenGL");
  info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  info.enabledExtensionCount = extensions.size();
  info.enabledExtensionNames = extensions.data();
  XrInstance result{};
  XR(xrCreateInstance(&info, &result));
  return result;
}
Pose pose(XrPosef p) {
  return {{p.position.x, p.position.y, p.position.z},
          {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w},
          1};
}
struct XrObject {
  EglContext graphics;
  Renderer renderer;
  Loader loader;
  XrInstance xr{};
  XrSystemId system{};
  XrSession session{};
  XrSpace reference{}, head{};
  XrActionSet actions{};
  XrAction grip{}, aim{}, squeeze{}, reset{}, close{}, haptic{};
  XrPath hands[2]{};
  XrSpace grips[2]{}, aims[2]{};
  struct Eye {
    XrSwapchain swapchain{};
    int width = 0, height = 0;
    GLuint fbo = 0, depth = 0;
    std::vector<XrSwapchainImageOpenGLKHR> images;
  };
  std::vector<Eye> eyes;
  bool running = false, quit = false, focused = false, placed = false,
       overlay = true;
  XrEnvironmentBlendMode blend = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  std::shared_ptr<const Model> model;
  Manipulator manipulation;
  size_t frames = 0, rendered = 0, tracked[2]{}, grabFrames = 0,
         focusedFrames = 0;
  std::array<Hand, 2> input{};
  explicit XrObject(std::shared_ptr<const Model> value, bool exclusive)
      : model(std::move(value)), overlay(!exclusive) {
    try {
      setup();
      loader.open(model->path);
    } catch (...) {
      cleanup();
      throw;
    }
  }
  ~XrObject() { cleanup(); }
  void cleanup() noexcept {
    for (auto &e : eyes) {
      if (e.fbo)
        glDeleteFramebuffers(1, &e.fbo);
      if (e.depth)
        glDeleteRenderbuffers(1, &e.depth);
      if (e.swapchain)
        xrDestroySwapchain(e.swapchain);
    }
    eyes.clear();
    for (int i = 0; i < 2; ++i) {
      if (grips[i])
        xrDestroySpace(grips[i]);
      if (aims[i])
        xrDestroySpace(aims[i]);
      grips[i] = aims[i] = XR_NULL_HANDLE;
    }
    if (head)
      xrDestroySpace(head);
    if (reference)
      xrDestroySpace(reference);
    head = reference = XR_NULL_HANDLE;
    if (session)
      xrDestroySession(session);
    session = XR_NULL_HANDLE;
    if (actions)
      xrDestroyActionSet(actions);
    actions = XR_NULL_HANDLE;
    if (xr)
      xrDestroyInstance(xr);
    xr = XR_NULL_HANDLE;
  }
  void setup() {
    auto available = extensions();
    std::vector<const char *> enabled{XR_KHR_OPENGL_ENABLE_EXTENSION_NAME,
                                      XR_MNDX_EGL_ENABLE_EXTENSION_NAME};
    if (overlay)
      enabled.push_back(XR_EXTX_OVERLAY_EXTENSION_NAME);
    for (auto name : enabled)
      if (!has(available, name))
        throw std::runtime_error(std::string("XR glTF viewing requires ") +
                                 name);
    auto profiles = controllerProfiles(available);
    for (auto &p : profiles)
      if (!p.extension.empty())
        enabled.push_back(p.extension.c_str());
    xr = instance(enabled);
    XrSystemGetInfo get{XR_TYPE_SYSTEM_GET_INFO};
    get.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR(xrGetSystem(xr, &get, &system));
    PFN_xrGetOpenGLGraphicsRequirementsKHR requirements{};
    XR(xrGetInstanceProcAddr(
        xr, "xrGetOpenGLGraphicsRequirementsKHR",
        reinterpret_cast<PFN_xrVoidFunction *>(&requirements)));
    XrGraphicsRequirementsOpenGLKHR req{
        XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
    XR(requirements(xr, system, &req));
    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    auto version = XR_MAKE_VERSION(major, minor, 0);
    if (version < req.minApiVersionSupported ||
        version > req.maxApiVersionSupported)
      throw std::runtime_error(
          "OpenGL context is outside runtime requirements");
    XrSessionCreateInfoOverlayEXTX layer{
        XR_TYPE_SESSION_CREATE_INFO_OVERLAY_EXTX};
    layer.sessionLayersPlacement = 30;
    XrGraphicsBindingEGLMNDX binding{XR_TYPE_GRAPHICS_BINDING_EGL_MNDX};
    binding.next = overlay ? &layer : nullptr;
    binding.display = graphics.display;
    binding.config = graphics.config;
    binding.context = graphics.context;
    binding.getProcAddress =
        reinterpret_cast<PFN_xrEglGetProcAddressMNDX>(eglGetProcAddress);
    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
    info.next = &binding;
    info.systemId = system;
    XR(xrCreateSession(xr, &info, &session));
    XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    space.poseInReferenceSpace.orientation.w = 1;
    XR(xrCreateReferenceSpace(session, &space, &reference));
    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XR(xrCreateReferenceSpace(session, &space, &head));
    XrActionSetCreateInfo set{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(set.actionSetName, "fsvr_object");
    strcpy(set.localizedActionSetName, "FSVR object controls");
    XR(xrCreateActionSet(xr, &set, &actions));
    XR(xrStringToPath(xr, "/user/hand/left", &hands[0]));
    XR(xrStringToPath(xr, "/user/hand/right", &hands[1]));
    auto action = [&](const char *name, XrActionType type, XrAction &out) {
      XrActionCreateInfo a{XR_TYPE_ACTION_CREATE_INFO};
      strcpy(a.actionName, name);
      strcpy(a.localizedActionName, name);
      a.actionType = type;
      a.countSubactionPaths = 2;
      a.subactionPaths = hands;
      XR(xrCreateAction(actions, &a, &out));
    };
    action("grip", XR_ACTION_TYPE_POSE_INPUT, grip);
    action("aim", XR_ACTION_TYPE_POSE_INPUT, aim);
    action("squeeze", XR_ACTION_TYPE_FLOAT_INPUT, squeeze);
    action("reset", XR_ACTION_TYPE_BOOLEAN_INPUT, reset);
    action("close", XR_ACTION_TYPE_BOOLEAN_INPUT, close);
    action("haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, haptic);
    const std::map<std::string, XrAction> map{
        {"grip", grip},   {"aim", aim},     {"squeeze", squeeze},
        {"reset", reset}, {"close", close}, {"haptic", haptic}};
    for (auto &profile : profiles) {
      std::vector<XrActionSuggestedBinding> bindings;
      for (auto &b : controllerBindings(profile)) {
        XrPath path;
        XR(xrStringToPath(xr, b.path.c_str(), &path));
        bindings.push_back({map.at(b.action), path});
      }
      XrInteractionProfileSuggestedBinding suggest{
          XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
      XR(xrStringToPath(xr, profile.path.c_str(), &suggest.interactionProfile));
      suggest.countSuggestedBindings = bindings.size();
      suggest.suggestedBindings = bindings.data();
      auto result = xrSuggestInteractionProfileBindings(xr, &suggest);
      if (XR_FAILED(result) && profile.touch && profile.extension.empty())
        check(result, "Default Touch controller bindings");
    }
    XrSessionActionSetsAttachInfo attach{
        XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &actions;
    XR(xrAttachSessionActionSets(session, &attach));
    for (int i = 0; i < 2; ++i) {
      XrActionSpaceCreateInfo a{XR_TYPE_ACTION_SPACE_CREATE_INFO};
      a.subactionPath = hands[i];
      a.poseInActionSpace.orientation.w = 1;
      a.action = grip;
      XR(xrCreateActionSpace(session, &a, &grips[i]));
      a.action = aim;
      XR(xrCreateActionSpace(session, &a, &aims[i]));
    }
    uint32_t n = 0;
    XR(xrEnumerateEnvironmentBlendModes(
        xr, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr));
    std::vector<XrEnvironmentBlendMode> modes(n);
    XR(xrEnumerateEnvironmentBlendModes(
        xr, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n,
        modes.data()));
    if (modes.empty())
      throw std::runtime_error("No XR environment blend modes");
    blend = std::find(modes.begin(), modes.end(),
                      XR_ENVIRONMENT_BLEND_MODE_OPAQUE) != modes.end()
                ? XR_ENVIRONMENT_BLEND_MODE_OPAQUE
                : modes[0];
    XR(xrEnumerateViewConfigurationViews(
        xr, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr));
    if (n != 2)
      throw std::runtime_error("XR object viewer requires stereo views");
    std::vector<XrViewConfigurationView> views(
        n, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    XR(xrEnumerateViewConfigurationViews(
        xr, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n,
        views.data()));
    XR(xrEnumerateSwapchainFormats(session, 0, &n, nullptr));
    std::vector<int64_t> formats(n);
    XR(xrEnumerateSwapchainFormats(session, n, &n, formats.data()));
    int64_t format = std::find(formats.begin(), formats.end(),
                               GL_SRGB8_ALPHA8) != formats.end()
                         ? GL_SRGB8_ALPHA8
                         : GL_RGBA8;
    if (std::find(formats.begin(), formats.end(), format) == formats.end())
      throw std::runtime_error("RGBA XR swapchains unavailable");
    eyes.resize(2);
    for (int i = 0; i < 2; ++i) {
      auto &e = eyes[i];
      e.width = views[i].recommendedImageRectWidth;
      e.height = views[i].recommendedImageRectHeight;
      if (e.width < 1 || e.height < 1 || e.width > 8192 || e.height > 8192)
        throw std::runtime_error("Invalid XR view dimensions");
      XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
      sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
      sc.format = format;
      sc.sampleCount = 1;
      sc.width = e.width;
      sc.height = e.height;
      sc.faceCount = sc.arraySize = sc.mipCount = 1;
      XR(xrCreateSwapchain(session, &sc, &e.swapchain));
      XR(xrEnumerateSwapchainImages(e.swapchain, 0, &n, nullptr));
      e.images.resize(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
      XR(xrEnumerateSwapchainImages(
          e.swapchain, n, &n,
          reinterpret_cast<XrSwapchainImageBaseHeader *>(e.images.data())));
      glGenFramebuffers(1, &e.fbo);
      glGenRenderbuffers(1, &e.depth);
      glBindRenderbuffer(GL_RENDERBUFFER, e.depth);
      glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, e.width,
                            e.height);
    }
  }
  void events() {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    XrResult result;
    while ((result = xrPollEvent(xr, &event)) == XR_SUCCESS) {
      if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
        auto state =
            reinterpret_cast<XrEventDataSessionStateChanged &>(event).state;
        focused = state == XR_SESSION_STATE_FOCUSED;
        if (!focused)
          manipulation.release();
        if (state == XR_SESSION_STATE_READY) {
          XrSessionBeginInfo b{XR_TYPE_SESSION_BEGIN_INFO};
          b.primaryViewConfigurationType =
              XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
          XR(xrBeginSession(session, &b));
          running = true;
        } else if (state == XR_SESSION_STATE_STOPPING) {
          XR(xrEndSession(session));
          running = false;
        } else if (state == XR_SESSION_STATE_EXITING ||
                   state == XR_SESSION_STATE_LOSS_PENDING)
          quit = true;
      } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
        quit = true;
      else if (event.type ==
               XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
        manipulation.release();
        placed = false;
      }
      event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    if (result != XR_EVENT_UNAVAILABLE)
      check(result, "xrPollEvent");
  }
  bool locate(XrSpace space, XrTime time, Pose &out) {
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    XR(xrLocateSpace(space, reference, time, &location));
    auto flags = XR_SPACE_LOCATION_POSITION_VALID_BIT |
                 XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if ((location.locationFlags & flags) != flags)
      return false;
    out = pose(location.pose);
    return finite(out);
  }
  void controllers(XrTime time) {
    input = {};
    Pose headPose;
    if (locate(head, time, headPose)) {
      if (!placed) {
        manipulation.reset(headPose);
        placed = true;
      }
    } else {
      manipulation.release();
      return;
    }
    XrActiveActionSet active{actions, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    auto result = xrSyncActions(session, &sync);
    if (result == XR_SESSION_NOT_FOCUSED) {
      manipulation.release();
      return;
    }
    check(result, "xrSyncActions");
    auto activePose = [&](XrAction action, int i) {
      XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
      get.action = action;
      get.subactionPath = hands[i];
      XrActionStatePose state{XR_TYPE_ACTION_STATE_POSE};
      XR(xrGetActionStatePose(session, &get, &state));
      return state.isActive;
    };
    auto clicked = [&](XrAction action, int i) {
      XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
      get.action = action;
      get.subactionPath = hands[i];
      XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
      XR(xrGetActionStateBoolean(session, &get, &state));
      return state.isActive && state.currentState && state.changedSinceLastSync;
    };
    for (int i = 0; i < 2; ++i) {
      auto &h = input[i];
      h.valid = activePose(grip, i) && locate(grips[i], time, h.grip);
      h.aimValid = activePose(aim, i) && locate(aims[i], time, h.aim);
      XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
      get.action = squeeze;
      get.subactionPath = hands[i];
      XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
      XR(xrGetActionStateFloat(session, &get, &state));
      h.squeeze = state.isActive ? state.currentState : 0;
      if (h.valid)
        ++tracked[i];
    }
    if (clicked(reset, 0))
      manipulation.reset(headPose);
    if (clicked(close, 1)) {
      quit = true;
      manipulation.release();
      return;
    }
    int previous = manipulation.grabbing();
    manipulation.update(input, model->normalizedBounds(), focused);
    if (manipulation.grabbing())
      ++grabFrames;
    for (int i = 0; i < 2; ++i)
      if ((manipulation.grabbing() & (1 << i)) && !(previous & (1 << i))) {
        XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
        info.action = haptic;
        info.subactionPath = hands[i];
        XrHapticVibration pulse{XR_TYPE_HAPTIC_VIBRATION};
        pulse.duration = 25000000;
        pulse.frequency = XR_FREQUENCY_UNSPECIFIED;
        pulse.amplitude = .3f;
        xrApplyHapticFeedback(
            session, &info,
            reinterpret_cast<const XrHapticBaseHeader *>(&pulse));
      }
  }
  void run(double seconds, const std::filesystem::path &report) {
    auto started = std::chrono::steady_clock::now();
    auto deadline = seconds > 0
                        ? started + std::chrono::duration_cast<
                                        std::chrono::steady_clock::duration>(
                                        std::chrono::duration<double>(seconds))
                        : std::chrono::steady_clock::time_point::max();
    std::cout << Json{{"event", "ready"},
                      {"overlay", overlay},
                      {"message", "Grip: grab/move/turn. Two grips: "
                                  "scale/turn. X: reset. B: close."}}
                     .dump()
              << std::endl;
    while (!quit && std::chrono::steady_clock::now() < deadline) {
      events();
      if (!running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      auto update = loader.snapshot();
      if (update.model && update.model != model) {
        model = update.model;
        std::cout << Json{{"event", "reloaded"},
                          {"message", "XR model reloaded"}}
                         .dump()
                  << std::endl;
      }
      XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
      XrFrameState frame{XR_TYPE_FRAME_STATE};
      XR(xrWaitFrame(session, &wait, &frame));
      XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
      XR(xrBeginFrame(session, &begin));
      ++frames;
      if (focused)
        ++focusedFrames;
      controllers(frame.predictedDisplayTime);
      std::vector<XrCompositionLayerProjectionView> views(
          eyes.size(), {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
      XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
      layer.space = reference;
      layer.layerFlags =
          overlay ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
      layer.viewCount = views.size();
      layer.views = views.data();
      bool submit = false;
      if (frame.shouldRender && placed) {
        XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
        locateInfo.viewConfigurationType =
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locateInfo.displayTime = frame.predictedDisplayTime;
        locateInfo.space = reference;
        XrViewState state{XR_TYPE_VIEW_STATE};
        std::vector<XrView> poses(eyes.size(), {XR_TYPE_VIEW});
        uint32_t n = 0;
        XR(xrLocateViews(session, &locateInfo, &state, poses.size(), &n,
                         poses.data()));
        auto flags = XR_VIEW_STATE_POSITION_VALID_BIT |
                     XR_VIEW_STATE_ORIENTATION_VALID_BIT;
        if (n == eyes.size() && (state.viewStateFlags & flags) == flags) {
          submit = true;
          renderer.prepare(model,
                           std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - started)
                               .count());
          for (size_t i = 0; i < eyes.size(); ++i) {
            auto &e = eyes[i];
            uint32_t index;
            XrSwapchainImageAcquireInfo acq{
                XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            XR(xrAcquireSwapchainImage(e.swapchain, &acq, &index));
            XrSwapchainImageWaitInfo ready{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            ready.timeout = XR_INFINITE_DURATION;
            XR(xrWaitSwapchainImage(e.swapchain, &ready));
            glBindFramebuffer(GL_FRAMEBUFFER, e.fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, e.images.at(index).image, 0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                      GL_RENDERBUFFER, e.depth);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
                GL_FRAMEBUFFER_COMPLETE)
              throw std::runtime_error("XR glTF "
                                       "framebuffer "
                                       "incomplete");
            auto p = pose(poses[i].pose);
            auto f = poses[i].fov;
            auto vp =
                perspective(f.angleLeft, f.angleRight, f.angleDown, f.angleUp) *
                matrix(inverse(p));
            renderer.draw(vp, matrix(manipulation.object) * model->bounds.fit(),
                          p.position, e.width, e.height,
                          overlay ? std::array<float, 4>{0, 0, 0, 0}
                                  : std::array<float, 4>{.03, .04, .05, 1});
            renderer.controllers(vp, input, manipulation.object,
                                 model->normalizedBounds(),
                                 manipulation.grabbing());
            glFlush();
            XrSwapchainImageReleaseInfo release{
                XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            XR(xrReleaseSwapchainImage(e.swapchain, &release));
            views[i].pose = poses[i].pose;
            views[i].fov = f;
            views[i].subImage.swapchain = e.swapchain;
            views[i].subImage.imageRect.extent = {e.width, e.height};
          }
          ++rendered;
        }
      }
      const XrCompositionLayerBaseHeader *base =
          reinterpret_cast<const XrCompositionLayerBaseHeader *>(&layer);
      XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
      end.displayTime = frame.predictedDisplayTime;
      end.environmentBlendMode = blend;
      end.layerCount = submit ? 1 : 0;
      end.layers = submit ? &base : nullptr;
      XR(xrEndFrame(session, &end));
    }
    if (!report.empty()) {
      auto &p = manipulation.object;
      std::ofstream(report)
          << Json{{"frames", frames},
                  {"rendered_stereo_frames", rendered},
                  {"tracked_left", tracked[0]},
                  {"tracked_right", tracked[1]},
                  {"focused_frames", focusedFrames},
                  {"grab_frames", grabFrames},
                  {"overlay", overlay},
                  {"position", {p.position.x, p.position.y, p.position.z}},
                  {"orientation",
                   {p.orientation.x, p.orientation.y, p.orientation.z,
                    p.orientation.w}},
                  {"scale", p.scale}}
                 .dump(2)
          << '\n';
    }
  }
};
#endif
} // namespace
XrProbe probeXr() {
  XrProbe out;
#if FSVR_MODEL_XR
  XrInstance xr{};
  try {
    auto available = extensions();
    for (auto name :
         {XR_KHR_OPENGL_ENABLE_EXTENSION_NAME,
          XR_MNDX_EGL_ENABLE_EXTENSION_NAME, XR_EXTX_OVERLAY_EXTENSION_NAME})
      if (!has(available, name))
        throw std::runtime_error(std::string("Runtime lacks ") + name);
    xr = instance({});
    XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
    XR(xrGetInstanceProperties(xr, &props));
    out.runtime = props.runtimeName;
    XrSystemGetInfo get{XR_TYPE_SYSTEM_GET_INFO};
    get.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system;
    XR(xrGetSystem(xr, &get, &system));
    out.available = true;
    out.message = "Open model in XR with Touch/Quest controls";
  } catch (const std::exception &e) {
    std::cerr << "FSVR XR probe: " << e.what() << '\n';
    out.message = "Connect a headset and start the XR runtime to open models "
                  "in XR.";
  }
  if (xr)
    xrDestroyInstance(xr);
#else
  out.message = "FSVR was built without model XR support";
#endif
  return out;
}
int runXr(const std::filesystem::path &path, double seconds,
          const std::filesystem::path &report, bool exclusive) {
#if FSVR_MODEL_XR
  try {
    if (!std::isfinite(seconds) || seconds < 0)
      throw std::runtime_error("XR duration must be finite and nonnegative");
    auto model = load(path);
    XrObject object(model, exclusive);
    object.run(seconds, report);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FSVR XR: " << e.what() << '\n';
    return 1;
  }
#else
  std::cerr << "FSVR was built without model XR support\n";
  return 1;
#endif
}
void renderModel(const std::filesystem::path &path,
                 const std::filesystem::path &ppm, double seconds,
                 int animation) {
  if (!std::isfinite(seconds) || seconds < 0)
    throw std::runtime_error("Animation time must be finite and nonnegative");
  auto model = load(path);
  EglContext graphics;
  Renderer renderer;
  renderer.prepare(model, seconds, animation);
  GLuint fbo = 0, color = 0, depth = 0;
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glGenTextures(1, &color);
  glBindTexture(GL_TEXTURE_2D, color);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 512, 512, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, nullptr);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         color, 0);
  glGenRenderbuffers(1, &depth);
  glBindRenderbuffer(GL_RENDERBUFFER, depth);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, 512, 512);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                            GL_RENDERBUFFER, depth);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    throw std::runtime_error("glTF capture framebuffer incomplete");
  Pose camera{{0, 0, 1.8f}, {}, 1};
  auto vp = perspective(-.45, .45, -.45, .45) * matrix(inverse(camera));
  renderer.draw(vp, model->bounds.fit(), camera.position, 512, 512);
  savePpm(ppm, 512, 512);
  glDeleteFramebuffers(1, &fbo);
  glDeleteTextures(1, &color);
  glDeleteRenderbuffers(1, &depth);
}
} // namespace fsv::gltf
