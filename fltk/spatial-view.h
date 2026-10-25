// SPDX-License-Identifier: MIT
#pragma once
#include "appearance.h"
#include "model.h"
#include "thumbnails.h"
#include <FL/Fl_Gl_Window.H>
#include "object-math.h"
#include <FL/Fl_Shared_Image.H>
#include <FL/gl.h>
#include <GL/glu.h>
#include <functional>
#include <map>
struct ViewState {
  double zoom = 1, yaw = 0, pitch = 0, panX = 0, panZ = 0;
};
class SpatialView : public Fl_Widget {
  const fsv::Scene *scene = nullptr;
  int selected = -1, downX = 0, downY = 0, button = 0;
  bool dragged = false;
  double model[16]{}, projection[16]{};
  int viewport[4]{};
  std::string theme;
  std::map<std::string, GLuint> labels;
  fsv::Thumbnails thumbnails;
  std::map<std::filesystem::path,std::string> pendingLabels;
  void setup();
  void drawScene();
  void cube(const fsv::Entry &, bool root = false);
  GLuint label(const fsv::Entry &);
  void textQuad(const fsv::Entry &);

public:
  ViewState view;
  bool stereoPicking = false;
  fsv::gltf::Mat4 stereoProjection;
  double stereoModel[16]{};
  std::function<fsv::gltf::Vec3(float,float)> stereoSurface;

  Fl_Gl_Window *owner() const { return dynamic_cast<Fl_Gl_Window *>(window()); }
  void make_current() { if (owner()) owner()->make_current(); }
  void *context() const { return owner() ? owner()->context() : nullptr; }
  float pixels_per_unit() const { return owner() ? owner()->pixels_per_unit() : 1; }
  int pixel_w() const { return int(w() * pixels_per_unit()); }
  int pixel_h() const { return int(h() * pixels_per_unit()); }
  void flush() { make_current(); draw(); }
  void renderEye(const fsv::gltf::Mat4 &, float, float, float, int, int);

  std::function<void(int)> selectionChanged, openRequested, contextRequested;
  SpatialView(int x, int y, int w, int h);
  ~SpatialView();
  void setScene(const fsv::Scene *value) {
    scene = value;
    selected = -1;
    thumbnails.clear();
    pendingLabels.clear();
    invalidateLabels();
    redraw();
  }
  void setSelected(int value) {
    selected = value;
    redraw();
  }
  int selectedIndex() const { return selected; }
  void invalidateLabels();
  void pollThumbnails();
  // Coordinates are local FLTK units within this viewport.
  int pickAt(int x, int y);
  bool projectPoint(fsv::Vec point, double &x, double &y);
  void reset() {
    view = {};
    redraw();
  }
  void focusSelection();
  void draw() override;
  int handle(int) override;
  bool snapshot(const std::filesystem::path &);
};
