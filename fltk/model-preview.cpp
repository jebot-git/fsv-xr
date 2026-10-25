// SPDX-License-Identifier: MIT
#define GL_GLEXT_PROTOTYPES
#include "model-preview.h"
#include <GL/glext.h>

#include <FL/fl_utf8.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
namespace {
using namespace fsv::gltf;
Quat axis(float angle, Vec3 v) {
  return {v.x * std::sin(angle / 2), v.y * std::sin(angle / 2),
          v.z * std::sin(angle / 2), std::cos(angle / 2)};
}
std::string menuText(std::string value) {
  for (char &c : value)
    if (static_cast<unsigned char>(c) < 32 || c == '/' || c == '|' ||
        c == '&' || c == '\\')
      c = ' ';
  return value;
}
} // namespace
ModelView::ModelView(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
Fl_Gl_Window *ModelView::owner() const {
  return dynamic_cast<Fl_Gl_Window *>(window());
}
void ModelView::make_current() {
  if (auto parent = owner()) parent->make_current();
}
void ModelView::releaseTarget() {
  if (framebuffer) glDeleteFramebuffers(1, &framebuffer);
  if (color) glDeleteTextures(1, &color);
  if (depth) glDeleteRenderbuffers(1, &depth);
  framebuffer = color = depth = 0;
  targetWidth = targetHeight = 0;
}
void ModelView::releaseContext() {
  auto parent = owner();
  if (parent && parent->context() && parent->context() == renderContext) {
    parent->make_current();
    renderer.clear(true);
    releaseTarget();
  } else {
    renderer.abandon();
    framebuffer = color = depth = 0;
    targetWidth = targetHeight = 0;
  }
  renderContext = nullptr;
}
ModelView::~ModelView() { releaseContext(); }
void ModelView::allocateTarget(int width, int height) {
  if (framebuffer && width == targetWidth && height == targetHeight) return;
  releaseTarget();
  glGenFramebuffers(1, &framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  glGenTextures(1, &color);
  glBindTexture(GL_TEXTURE_2D, color);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
  glGenRenderbuffers(1, &depth);
  glBindRenderbuffer(GL_RENDERBUFFER, depth);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    throw std::runtime_error("Cannot allocate model preview framebuffer");
  targetWidth = width; targetHeight = height;
}
void ModelView::redrawScene() {
  // A regular widget invalidates the same surface that DomeD sends to XR.
  redraw();
}
void ModelView::setModel(std::shared_ptr<const Model> value) {
  if (value != model) {
    if (!value) releaseContext();
    model = std::move(value);
    error.clear();
    seconds = 0;
    redrawScene();
  }
}
void ModelView::reset() {
  yaw = pitch = panX = panY = 0;
  distance = 1.8f;
  error.clear();
  redrawScene();
}
void ModelView::advance(double dt) {
  if (!visible_r() || !model)
    return;
  if (playing)
    seconds += dt;
  if (orbit)
    yaw += dt * .18;
  if (orbit || (playing && !model->animations.empty()))
    redrawScene();
}
void ModelView::draw() {
  auto parent = owner();
  if (!parent) return;
  if (renderContext != parent->context() || !parent->context_valid()) {
    renderer.abandon();
    framebuffer = color = depth = 0;
    targetWidth = targetHeight = 0;
    renderContext = parent->context();
  }
  const double scale = parent->pixels_per_unit();
  const int width = std::max(1, int(std::lround(w() * scale)));
  const int height = std::max(1, int(std::lround(h() * scale)));
  GLint drawBuffer, readBuffer, program, vao, arrayBuffer, renderbuffer, unpackAlignment;
  GLint samplers[5];
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawBuffer);
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readBuffer);
  glGetIntegerv(GL_CURRENT_PROGRAM, &program);
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
  glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackAlignment);
  for (int unit = 0; unit < 5; ++unit)
    glGetIntegeri_v(GL_SAMPLER_BINDING, unit, &samplers[unit]);
  glPushAttrib(GL_ALL_ATTRIB_BITS);
  unsigned char r, g, b;
  Fl::get_color(FL_BACKGROUND2_COLOR, r, g, b);
  try {
    allocateTarget(width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, width, height);
    glClearColor(r / 255.f, g / 255.f, b / 255.f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (model && error.empty()) {
      renderer.prepare(model, seconds, animation);
      float up = .45f, right = std::atan(std::tan(up) * float(width) / height);
      Pose camera{{panX, panY, distance}, {}, 1};
      auto vp = perspective(-right, right, -up, up) * matrix(inverse(camera));
      auto root = trs({}, axis(yaw, {0, 1, 0}) * axis(pitch, {1, 0, 0})) * model->bounds.fit();
      renderer.draw(vp, root, camera.position, width, height,
                    {r / 255.f, g / 255.f, b / 255.f, 1});
    }
    // Composite entirely on the GPU into the containing FLTK surface. A native
    // GL subwindow can be absent from retained XR DMA-BUF window frames.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawBuffer);
    glDisable(GL_SCISSOR_TEST);
    int left = int(std::lround(x() * scale));
    int bottom = parent->pixel_h() - int(std::lround((y() + h()) * scale));
    glBlitFramebuffer(0, 0, width, height, left, bottom, left + width, bottom + height,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
  } catch (const std::exception &e) {
    error = e.what();
  }
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawBuffer);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, readBuffer);
  glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
  glUseProgram(program);
  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, arrayBuffer);
  glPixelStorei(GL_UNPACK_ALIGNMENT, unpackAlignment);
  for (int unit = 0; unit < 5; ++unit)
    glBindSampler(unit, samplers[unit]);
  glPopAttrib();
}
int ModelView::handle(int event) {
  if (event == FL_PUSH) {
    button = Fl::event_button();
    downX = Fl::event_x();
    downY = Fl::event_y();
    take_focus();
    return 1;
  }
  if (event == FL_DRAG) {
    int x = Fl::event_x(), y = Fl::event_y(), dx = x - downX, dy = y - downY;
    downX = x;
    downY = y;
    if (button == FL_MIDDLE_MOUSE || Fl::event_state(FL_SHIFT)) {
      panX -= dx * distance / std::max(1, w());
      panY += dy * distance / std::max(1, h());
    } else {
      orbit = false;
      yaw += dx * .01f;
      pitch = std::clamp(pitch + dy * .01f, -1.5f, 1.5f);
    }
    redrawScene();
    return 1;
  }
  if (event == FL_RELEASE) {
    button = 0;
    return 1;
  }
  if (event == FL_MOUSEWHEEL) {
    distance =
        std::clamp(distance * std::exp(Fl::event_dy() * .12f), .15f, 50.f);
    redrawScene();
    return 1;
  }
  if ((event == FL_KEYDOWN || event == FL_SHORTCUT) && Fl::event_key() == 'r') {
    reset();
    return 1;
  }
  return Fl_Widget::handle(event);
}
void ModelPreview::tick(void *p) {
  auto self = static_cast<ModelPreview *>(p);
  self->poll();
  Fl::repeat_timeout(self->shown() ? 1. / 30 : .1, tick, p);
}
void ModelPreview::action(Fl_Widget *w, void *p) {
  auto self = static_cast<ModelPreview *>(p);
  auto m = dynamic_cast<Fl_Menu_ *>(w);
  self->command(m && m->mvalue() ? m->mvalue()->label()
                : w->label()     ? w->label()
                                 : "");
}
ModelPreview::ModelPreview()
    : dx::GpuWindow(800, 620, "glTF live preview — FSVR") {
  end();
  for (auto label :
       {"File/Open in XR", "File/Close XR", "File/Close preview",
        "View/Reset view", "View/Play animation", "View/Orbit model"})
    menu.add(label, std::string(label) == "File/Close preview" ? FL_META + 'w' : 0, action, this);
  for (auto button : {&openXR, &closeXR, &resetView, &play, &autoOrbit})
    button->callback(action, this);
  autoOrbit.type(FL_TOGGLE_BUTTON);
  autoOrbit.value(view.orbit);
  clip.callback(
      [](Fl_Widget *w, void *p) {
        auto self = static_cast<ModelPreview *>(p);
        self->view.animation = static_cast<Fl_Choice *>(w)->value();
        self->view.seconds = 0;
        self->view.redrawScene();
      },
      this);
  info.wrap(true);
  info.textfont(dx::font);
  info.textsize(dx::size);
  resizable(view);
  size_range(620, 460);
  callback([](Fl_Widget *w, void *) { w->hide(); });
  if (dx::dome())
    menu.hide();
  desktop = std::make_unique<DesktopMenu>(*this, menu);
  xclass("org.fsvxr.fsvr.Model");
  last = std::chrono::steady_clock::now();
  Fl::add_timeout(1. / 30, tick, this);
  resize(x(), y(), w(), h());
}
int ModelPreview::handle(int event) {
  if (event == FL_SHORTCUT && !menu.visible() && !Fl::modal() && menu.handle(event))
    return 1;
  return dx::GpuWindow::handle(event);
}
void ModelPreview::hide() {
  view.releaseContext();
  dx::GpuWindow::hide();
}
ModelPreview::~ModelPreview() {
  Fl::remove_timeout(tick, this);
  stopXR();
  if (childOutput >= 0)
    ::close(childOutput);
}
void ModelPreview::menus() {
  autoOrbit.value(view.orbit);
  bool ready = bool(view.asset());
  for (auto button : {&openXR}) {
    if (canOpenXR())
      button->activate();
    else
      button->deactivate();
  }
  if (child > 0)
    closeXR.activate();
  else
    closeXR.deactivate();
  for (auto button : {&resetView, &play, &autoOrbit}) {
    if (ready)
      button->activate();
    else
      button->deactivate();
  }
  dx::enable_menu(menu, "File/Open in XR", canOpenXR());
  dx::enable_menu(menu, "File/Close XR", child > 0);
}
void ModelPreview::open(const std::filesystem::path &path, bool showPreview,
                        bool inXR) {
  if (path != source) {
    source = path;
    loader.open(path);
    view.setModel({});
    view.reset();
    revision = 0;
    pendingXR = false;
    copy_label(
        (path.filename().string() + " — glTF live preview — FSVR").c_str());
  }
  pendingXR |= inXR;
  if (showPreview)
    show();
  poll();
}
void ModelPreview::clearPassive() {
  // A hidden inspector preview follows selection, while a user-opened window
  // or active XR object keeps its independent lifetime.
  if(shown() || child>0 || pendingXR) return;
  source.clear();
  loader.clear();
  view.setModel({});
  revision=0;
}
void ModelPreview::poll() {
  auto now = std::chrono::steady_clock::now();
  view.advance(
      std::clamp(std::chrono::duration<double>(now - last).count(), 0., .1));
  last = now;
  auto update = loader.snapshot();
  if (update.revision != revision) {
    revision = update.revision;
    if (update.model && update.model != view.asset()) {
      view.setModel(update.model);
      clip.clear();
      if (update.model->animations.empty())
        clip.add("Rest pose");
      else
        for (size_t i = 0; i < update.model->animations.size(); ++i)
          clip.add((std::to_string(i + 1) + ": " +
                    menuText(update.model->animations[i].name))
                       .c_str());
      clip.value(0);
      view.animation = 0;
    }
  }
  if (!source.empty() && !probing.valid() && now >= nextProbe) {
    std::promise<XrProbe> promise;
    probing = promise.get_future();
    std::thread([p = std::move(promise)]() mutable {
      try {
        p.set_value(probeXr());
      } catch (...) {
        p.set_value({false, "", "XR runtime unavailable"});
      }
    }).detach();
    nextProbe = now + std::chrono::seconds(5);
  }
  if (probing.valid() &&
      probing.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    xr = probing.get();
    openXR.tooltip(xr.message.c_str());
  }
  readChild();
  if (pendingXR && canOpenXR()) {
    pendingXR = false;
    startXR();
  }
  std::string text;
  if (update.model)
    text = std::to_string(update.model->triangleCount()) + " triangles · " +
           std::to_string(update.model->animations.size()) +
           " animations. Drag: orbit · wheel: zoom · middle-drag: "
           "pan.";
  else
    text = update.loading ? "Loading glTF in the background…"
                          : "Choose a glTF/GLB file for a live preview.";
  if (!update.error.empty())
    text += "\nReload: " + update.error;
  if (!view.failure().empty())
    text += "\nRender: " + view.failure();
  if (!xrMessage.empty())
    text += "\n" + xrMessage;
  else if (xr.available)
    text += "\nXR: grip to grab/move/turn · two grips to scale · X "
            "reset · B close.";
  else if (!xr.message.empty())
    text += "\nXR unavailable: " + xr.message;
  if (text != lastInfo) {
    lastInfo = text;
    info.value(text.c_str());
  }
  menus();
}
void ModelPreview::startXR() {
  if (!view.asset())
    return;
  stopXR();
  int pipes[2];
  if (pipe2(pipes, O_CLOEXEC)) {
    xrMessage = "Cannot create XR status pipe";
    return;
  }
  fcntl(pipes[0], F_SETFL, fcntl(pipes[0], F_GETFL) | O_NONBLOCK);
  auto executable = std::filesystem::read_symlink("/proc/self/exe").string();
  std::string file = view.asset()->path.string();
  std::vector<char *> args{executable.data(), const_cast<char *>("--model-xr"),
                           file.data(), nullptr};
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, pipes[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, pipes[0]);
  pid_t pid = -1;
  int result = posix_spawn(&pid, executable.c_str(), &actions, nullptr,
                           args.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  ::close(pipes[1]);
  if (result) {
    ::close(pipes[0]);
    xrMessage = "Cannot start XR object viewer";
    return;
  }
  child = pid;
  childOutput = pipes[0];
  childText.clear();
  xrMessage = "Starting stereoscopic XR object viewer…";
}
void ModelPreview::readChild() {
  if (childOutput >= 0) {
    char bytes[1024];
    for (int i = 0; i < 8; ++i) {
      auto n = ::read(childOutput, bytes, sizeof(bytes));
      if (n <= 0)
        break;
      childText.append(bytes, n);
      if (childText.size() > 16384)
        childText.erase(0, childText.size() - 16384);
    }
    for (size_t p; (p = childText.find('\n')) != std::string::npos;) {
      auto line = childText.substr(0, p);
      childText.erase(0, p + 1);
      try {
        auto message = Json::parse(line);
        xrMessage = message.value("message", line);
      } catch (...) {
        if (!line.empty())
          xrMessage = line;
      }
    }
  }
  if (child > 0) {
    int status = 0;
    auto result = waitpid(child, &status, WNOHANG);
    if (result == child) {
      if (WIFEXITED(status) && !WEXITSTATUS(status))
        xrMessage = "XR object closed";
      else if (WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM)
        xrMessage = "XR object closed";
      else if (xrMessage.empty())
        xrMessage = "XR viewer exited with an error";
      child = -1;
      if (childOutput >= 0)
        ::close(childOutput);
      childOutput = -1;
    }
  }
}
void ModelPreview::stopXR() {
  if (child > 0) {
    auto pid = child;
    kill(pid, SIGTERM);
    std::thread([pid] {
      int status;
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
    }).detach();
    child = -1;
    xrMessage = "XR object closed";
  }
  if (childOutput >= 0)
    ::close(childOutput);
  childOutput = -1;
}
void ModelPreview::command(const std::string &name) {
  if (name == "Open in XR") {
    pendingXR = true;
    poll();
  } else if (name == "Close XR") {
    pendingXR = false;
    stopXR();
  } else if (name == "Close preview")
    hide();
  else if (name == "Reset view")
    view.reset();
  else if (name == "Play animation" || name == "Pause" || name == "Play") {
    view.playing = !view.playing;
    play.label(view.playing ? "Pause" : "Play");
  } else if (name == "Orbit model" || name == "Orbit") {
    view.orbit = !view.orbit;
  }
}
void ModelPreview::resize(int X, int Y, int W, int H) {
  dx::GpuWindow::resize(X, Y, W, H);
  int top = menu.visible() ? 30 : 0;
  menu.resize(0, 0, W, top);
  openXR.resize(8, top + 6, 116, 32);
  closeXR.resize(132, top + 6, 100, 32);
  resetView.resize(240, top + 6, 90, 32);
  play.resize(338, top + 6, 80, 32);
  autoOrbit.resize(426, top + 6, 80, 32);
  clip.resize(96, top + 46, W - 104, 30);
  view.resize(8, top + 82, W - 16, H - top - 166);
  info.resize(8, H - 78, W - 16, 70);
}
