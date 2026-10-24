// SPDX-License-Identifier: MIT
#pragma once
#include <EGL/egl.h>
namespace fsv::gltf {
class EglContext {
  void cleanup() noexcept;

public:
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLConfig config{};
  EGLContext context = EGL_NO_CONTEXT;
  EGLSurface surface = EGL_NO_SURFACE;
  EglContext();
  ~EglContext();
  EglContext(const EglContext &) = delete;
};
} // namespace fsv::gltf
