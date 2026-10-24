// SPDX-License-Identifier: MIT
// In-window GPU transport and cylinder reprojection adapted from Comic Chat.
#pragma once
#define GL_GLEXT_PROTOTYPES 1
#include <FL/gl.h>
#include <GL/glext.h>
#include "domed.h"
#include "window-stereo.h"
#include <functional>
namespace fsv_stereo {
class RenderTarget {
  GLuint framebuffer = 0, texture = 0, depth = 0;
  int width = 0, height = 0;

public:
  void abandon() {
    framebuffer = texture = depth = 0;
    width = height = 0;
  }
  void release() {
    if (framebuffer)
      glDeleteFramebuffers(1, &framebuffer);
    if (texture)
      glDeleteTextures(1, &texture);
    if (depth)
      glDeleteRenderbuffers(1, &depth);
    abandon();
  }
  GLuint colorTexture() const { return texture; }
  GLuint get(int w, int h) {
    if (w == width && h == height && framebuffer)
      return framebuffer;
    release();
    width = w;
    height = h;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           texture, 0);
    glGenRenderbuffers(1, &depth);
    glBindRenderbuffer(GL_RENDERBUFFER, depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
      throw std::runtime_error("Incomplete stereo framebuffer");
    return framebuffer;
  }
};
class Compositor {
  RenderTarget chrome, eyeTarget;
public:
  DomeMode mode;
  float offsetX = 0, offsetY = 0, contentHeight = 0;
  size_t frames = 0, stereoFrames = 0;
  std::string error;
  int X=0, Y=0, W=1, H=1;
  int x() const { return X; } int y() const { return Y; }
  int w() const { return W; } int h() const { return H; }
  std::function<void(const fsv::gltf::Mat4 &,
                     std::function<fsv::gltf::Vec3(float,float)>)> picking;
  std::function<void(const fsv::gltf::Mat4 &, float,float,float,int,int)> render;
  void abandon() { chrome.abandon(); eyeTarget.abandon(); }
  void release() { chrome.release(); eyeTarget.release(); }
  void composite(int logicalW, int logicalH, int pixelsW, int pixelsH,
                 bool renderScene = true) {
    using namespace fsv::gltf;
    GLint original = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &original);
    GLint savedMatrix; glGetIntegerv(GL_MATRIX_MODE, &savedMatrix);
    glMatrixMode(GL_PROJECTION); glPushMatrix();
    glMatrixMode(GL_MODELVIEW); glPushMatrix();
    auto restoreMatrices = [&] {
      glMatrixMode(GL_MODELVIEW); glPopMatrix();
      glMatrixMode(GL_PROJECTION); glPopMatrix();
      glMatrixMode(savedMatrix);
    };
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_SCISSOR_TEST);
    try {
      bool stereo = mode.stereoSupported;
      // Copy all chrome once, then squeeze an identical copy into each eye.
      // Input stays in the unsqueezed logical window coordinate system.
      if (stereo) {
        GLuint copy = chrome.get(pixelsW, pixelsH);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, original);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, copy);
        glBlitFramebuffer(0, 0, pixelsW, pixelsH, 0, 0, pixelsW, pixelsH,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, copy);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, original);
        int half = pixelsW / 2;
        glBlitFramebuffer(0, 0, pixelsW, pixelsH, 0, 0, half, pixelsH,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBlitFramebuffer(0, 0, pixelsW, pixelsH, pixelsW - half, 0, pixelsW,
                          pixelsH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
      }
      if (!renderScene) {
        glBindFramebuffer(GL_FRAMEBUFFER, original);
        glPopAttrib();
        restoreMatrices();
        return;
      }
      int pw = std::max(1, w() * pixelsW / logicalW),
          ph = std::max(1, h() * pixelsH / logicalH);
      GLuint target = eyeTarget.get(pw, ph);
      float distance = mode.pulloutDistance;
      float windowH = distance * mode.laneHeightRadians;
      float windowW = windowH * logicalW / logicalH;
      if (mode.hasWindowQuad) {
        windowW = mode.windowSize[0];
        windowH = mode.windowSize[1];
      }
      // Restrict content to the lane even when a pulled-out window is taller.
      float laneFraction =
          std::min(1.f, distance * mode.laneHeightRadians / windowH);
      float height = windowH * float(h()) / logicalH;
      float width = windowW * float(w()) / logicalW;
      contentHeight = .96f * std::min(width, height * laneFraction);

      float centreX = windowW * ((x() + w() / 2.f) / logicalW - .5f);
      float centreY = windowH * (.5f - (y() + h() / 2.f) / logicalH);
      CylinderSurface cylinder;
      fsv::gltf::Pose tangent;
      if (mode.hasCylinder) {
        auto p = mode.cylinderPosition;
        auto q = mode.cylinderOrientation;
        cylinder = {{{p[0], p[1], p[2]}, {q[0], q[1], q[2], q[3]}, 1},
                    mode.cylinderRadius,
                    mode.cylinderHorizontal,
                    mode.cylinderTop,
                    mode.cylinderBottom};
        tangent = cylinder.tangent((x() + w() / 2.f) / logicalW,
                                   (y() + h() / 2.f) / logicalH);
      }
      auto surfacePoint = [&](float u, float v) {
        auto p = cylinder.point((x() + u * w()) / logicalW,
                                (y() + v * h()) / logicalH);
        auto local = transform(inverse(tangent), p);
        return Vec3{local.x, 1.6f + local.y, local.z - distance};
      };
      int count = stereo ? 2 : 1;

      for (int eye = 0; eye < count; ++eye) {
        Vec3 relative{stereo ? (eye ? .032f : -.032f) : 0, 0, 0};
        if (stereo && mode.hasWindowQuad) {
          auto p = mode.windowPosition;
          auto q = mode.windowOrientation;
          fsv::gltf::Pose panel{
              {p[0], p[1], p[2]}, {q[0], q[1], q[2], q[3]}, 1};
          auto e = mode.eyes[eye];
          Vec3 worldEye{e[0], e[1], e[2]};
          if (!mode.trackedEyes) {
            auto p = mode.headPosition;
            auto q = mode.headOrientation;
            fsv::gltf::Pose head{
                {p[0], p[1], p[2]}, {q[0], q[1], q[2], q[3]}, 1};
            worldEye = transform(head, {eye ? .032f : -.032f, 0, 0});
          }
          auto local =
              transform(inverse(mode.hasCylinder ? tangent : panel), worldEye);
          relative = {local.x - (mode.hasCylinder ? 0 : centreX),
                      local.y - (mode.hasCylinder ? 0 : centreY),
                      local.z - distance};
        }
        relative.x += offsetX;
        relative.y += offsetY;
        auto vp = windowEyeProjection(relative, width, height, distance);
        if (mode.hasCylinder) {
          float left = 1e9f, right = -1e9f, down = 1e9f, up = -1e9f;
          Vec3 eyePosition{relative.x, 1.6f + relative.y, relative.z};
          for (int row = 0; row <= 8; ++row)
            for (int col = 0; col <= 32; ++col) {
              auto ray = surfacePoint(col / 32.f, row / 8.f) - eyePosition;
              if (ray.z >= -.02f)
                continue;
              left = std::min(left, ray.x / -ray.z);
              right = std::max(right, ray.x / -ray.z);
              down = std::min(down, ray.y / -ray.z);
              up = std::max(up, ray.y / -ray.z);
            }
          if (!(left < right && down < up))
            throw std::runtime_error("Window surface is behind the viewer");
          vp = perspective(std::atan(left), std::atan(right), std::atan(down),
                           std::atan(up)) *
               matrix(inverse(fsv::gltf::Pose{eyePosition, {}, 1}));
        }
        if (picking && eye == 0) {
          auto surface = mode.hasCylinder ? std::function<Vec3(float,float)>(surfacePoint)
              : std::function<Vec3(float,float)>([=](float u, float v) {
                  return Vec3{(u-.5f)*width, 1.6f+(.5f-v)*height, -distance};
                });
          // Evaluate the surface now: surfacePoint refers to this frame's stack.
          std::array<Vec3, 33*33> mesh;
          for (int row=0;row<=32;++row) for (int col=0;col<=32;++col)
            mesh[row*33+col]=surface(col/32.f,row/32.f);
          picking(vp,
                  [mesh](float u,float v) {
                    float x=std::clamp(u,0.f,1.f)*32, y=std::clamp(v,0.f,1.f)*32;
                    int c=std::min(31,int(x)), r=std::min(31,int(y));
                    float a=x-c,b=y-r;
                    return (mesh[r*33+c]*(1-a)+mesh[r*33+c+1]*a)*(1-b)
                         + (mesh[(r+1)*33+c]*(1-a)+mesh[(r+1)*33+c+1]*a)*b;
                  });
        }
        glBindFramebuffer(GL_FRAMEBUFFER, target);
        render(vp, width, height, distance, pw, ph);
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, target);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, original);
        int left = x() * pixelsW / logicalW,
            right = (x() + w()) * pixelsW / logicalW;
        if (stereo) {
          int half = pixelsW / 2, base = eye ? pixelsW - half : 0;
          left = base + left * half / pixelsW;
          right = base + right * half / pixelsW;
        }
        int bottom = (logicalH - y() - h()) * pixelsH / logicalH,
            top = (logicalH - y()) * pixelsH / logicalH;
        if (!mode.hasCylinder) {
          glBlitFramebuffer(0, 0, pw, ph, left, bottom, right, top,
                            GL_COLOR_BUFFER_BIT, GL_LINEAR);
        } else {
          // Reproject each eye through the physical cylinder. Only this content
          // rectangle is warped; chrome retains the compositor's normal UVs.
          glBindFramebuffer(GL_FRAMEBUFFER, original);
          glViewport(0, 0, pixelsW, pixelsH);
          glUseProgram(0);
          glBindVertexArray(0);
          glActiveTexture(GL_TEXTURE0);
          glBindSampler(0, 0);
          glEnable(GL_TEXTURE_2D);
          glBindTexture(GL_TEXTURE_2D, eyeTarget.colorTexture());
          glDisable(GL_DEPTH_TEST);
          glDisable(GL_CULL_FACE);
          glDisable(GL_BLEND);
          glColor4f(1, 1, 1, 1);
          GLint previousMatrix;
          glGetIntegerv(GL_MATRIX_MODE, &previousMatrix);
          glMatrixMode(GL_PROJECTION);
          glPushMatrix();
          glLoadIdentity();
          glOrtho(0, pixelsW, 0, pixelsH, -1, 1);
          glMatrixMode(GL_MODELVIEW);
          glPushMatrix();
          glLoadIdentity();
          auto vertex = [&](float u, float v) {
            auto p = point(vp, surfacePoint(u, v));
            glTexCoord2f((p.x + 1) / 2, (p.y + 1) / 2);
            glVertex2f(left + u * (right - left), top - v * (top - bottom));
          };
          glBegin(GL_TRIANGLES);
          for (int row = 0; row < 32; ++row)
            for (int col = 0; col < 32; ++col) {
              float u = col / 32.f, v = row / 32.f, a = (col + 1) / 32.f,
                    b = (row + 1) / 32.f;
              vertex(u, v);
              vertex(a, v);
              vertex(a, b);
              vertex(u, v);
              vertex(a, b);
              vertex(u, b);
            }
          glEnd();
          glPopMatrix();
          glMatrixMode(GL_PROJECTION);
          glPopMatrix();
          glMatrixMode(previousMatrix);
        }
      }
      ++frames;
      if (stereo) ++stereoFrames;
      error.clear();
    } catch (const std::exception &e) {
      error = e.what();
    }
    glBindFramebuffer(GL_FRAMEBUFFER, original);
    glUseProgram(0);
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
    glPopAttrib();
    restoreMatrices();
  }
};
}
