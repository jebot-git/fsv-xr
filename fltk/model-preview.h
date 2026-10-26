// SPDX-License-Identifier: MIT
#pragma once
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Multiline_Output.H>
#include <sys/types.h>

#include <future>

#include "desktop-menu.h"
#include "gltf-render.h"
#include "gpu-window.h"
#include "model-loader.h"
#include "model-xr.h"
class ModelView : public Fl_Widget {
  fsv::gltf::Renderer renderer;
  void *renderContext = nullptr;
  GLuint framebuffer = 0, color = 0, depth = 0;
  int targetWidth = 0, targetHeight = 0;
  Fl_Gl_Window *owner() const;
  void releaseTarget();
  void allocateTarget(int, int);
  std::shared_ptr<const fsv::gltf::Model> model;
  int downX = 0, downY = 0, button = 0;
  float yaw = 0, pitch = 0, distance = 1.8f, panX = 0, panY = 0;
  std::string error;

public:
  bool playing = true, orbit = true;
  int animation = 0;
  double seconds = 0;
  ModelView(int x, int y, int w, int h);
  ~ModelView();
  void setModel(std::shared_ptr<const fsv::gltf::Model>);
  const std::shared_ptr<const fsv::gltf::Model> &asset() const { return model; }
  void advance(double dt);
  void reset();
  void redrawScene();
  void make_current();
  void releaseContext();
  const std::string &failure() const { return error; }
  void draw() override;
  int handle(int) override;
};
class ModelPreview : public dx::GpuWindow {
  Fl_Menu_Bar menu{0, 0, 800, 30};
  Fl_Button openXR{8, 36, 116, 32, "Open in XR"},
      closeXR{132, 36, 100, 32, "Close XR"},
      resetView{240, 36, 90, 32, "Reset view"}, play{338, 36, 80, 32, "Pause"},
      autoOrbit{426, 36, 80, 32, "Orbit"};
  Fl_Choice clip{96, 76, 500, 30, "Animation"};
  ModelView view{8, 112, 784, 416};
  Fl_Multiline_Output info{8, 536, 784, 76};
  std::unique_ptr<DesktopMenu> desktop;
  fsv::gltf::Loader loader;
  std::filesystem::path source;
  uint64_t revision = 0;
  bool pendingXR = false;
  std::future<fsv::gltf::XrProbe> probing;
  fsv::gltf::XrProbe xr;
  std::chrono::steady_clock::time_point last{}, nextProbe{};
  pid_t child = -1;
  int childOutput = -1;
  std::string childText, xrMessage, lastInfo;
  static void tick(void *);
  static void action(Fl_Widget *, void *);
  void poll();
  void startXR();
  void stopXR();
  void readChild();
  void menus();

public:
  ModelPreview();
  ~ModelPreview();
  void hide() override;
  void open(const std::filesystem::path &, bool showPreview = true,
            bool inXR = false);
  void command(const std::string &);
  void clearPassive();
  void previewSelection(const std::filesystem::path &);
  int handle(int) override;
  bool canOpenXR() const { return xr.available && view.asset() != nullptr; }
  void resize(int, int, int, int) override;
  const std::filesystem::path &path() const { return source; }
  ModelView &modelView() { return view; }
};
