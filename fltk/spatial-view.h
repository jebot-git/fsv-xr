// SPDX-License-Identifier: MIT
#pragma once
#include "appearance.h"
#include "model.h"
#include <FL/Fl_Gl_Window.H>
#include <FL/Fl_Shared_Image.H>
#include <FL/gl.h>
#include <GL/glu.h>
#include <functional>
#include <map>
struct ViewState {
  double zoom = 1, yaw = 0, pitch = 0, panX = 0, panZ = 0;
};
class SpatialView : public Fl_Gl_Window {
  const fsv::Scene *scene = nullptr;
  int selected = -1, downX = 0, downY = 0, button = 0;
  bool dragged = false;
  double model[16]{}, projection[16]{};
  int viewport[4]{};
  std::string theme;
  std::map<std::string, GLuint> labels;
  void setup();
  void cube(const fsv::Entry &, bool root = false);
  GLuint label(const fsv::Entry &);
  void textQuad(const fsv::Entry &);

public:
  ViewState view;
  std::function<void(int)> selectionChanged, openRequested, contextRequested;
  SpatialView(int x, int y, int w, int h);
  ~SpatialView();
  void setScene(const fsv::Scene *value) {
    scene = value;
    invalidateLabels();
    redraw();
  }
  void setSelected(int value) {
    selected = value;
    redraw();
  }
  int selectedIndex() const { return selected; }
  void invalidateLabels();
  // Coordinates are local FLTK units, as supplied to this subwindow's handle().
  int pickAt(int x, int y);
  void reset() {
    view = {};
    redraw();
  }
  void focusSelection();
  void draw() override;
  int handle(int) override;
  bool snapshot(const std::filesystem::path &);
};
