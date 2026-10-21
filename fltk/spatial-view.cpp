// SPDX-License-Identifier: MIT
#include "spatial-view.h"
#include <FL/Fl_Box.H>
#include <FL/Fl_Image_Surface.H>
#include <cmath>
#include <cstring>
#include <fstream>
SpatialView::SpatialView(int x, int y, int w, int h)
    : Fl_Gl_Window(x, y, w, h) {
  mode(FL_RGB | FL_ALPHA | FL_DOUBLE | FL_DEPTH);
  end();
}
SpatialView::~SpatialView() {
  if (context()) {
    make_current();
    for (auto &label : labels)
      glDeleteTextures(1, &label.second);
  }
}
void SpatialView::invalidateLabels() {
  if (context()) {
    make_current();
    for (auto &label : labels)
      glDeleteTextures(1, &label.second);
  }
  labels.clear();
}
void SpatialView::setup() {
  glViewport(0, 0, pixel_w(), pixel_h());
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  gluPerspective(42, double(pixel_w()) / std::max(1, pixel_h()), 10, 2000);
  glGetDoublev(GL_PROJECTION_MATRIX, projection);
  glGetIntegerv(GL_VIEWPORT, viewport);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  gluLookAt(0, 170 / view.zoom, 230 / view.zoom, 0, 0, 0, 0, 1, 0);
  glTranslated(view.panX, 0, view.panZ);
  glRotated(view.pitch, 1, 0, 0);
  glRotated(view.yaw, 0, 1, 0);
  glGetDoublev(GL_MODELVIEW_MATRIX, model);
}
void SpatialView::cube(const fsv::Entry &e, bool root) {
  auto c = e.center, s = e.extent * .5;
  glPushMatrix();
  glTranslated(c.x, c.y, c.z);
  glScaled(s.x, s.y, s.z);
  glBegin(GL_QUADS);
  glNormal3d(0, 1, 0);
  glVertex3d(-1, 1, -1);
  glVertex3d(-1, 1, 1);
  glVertex3d(1, 1, 1);
  glVertex3d(1, 1, -1);
  glNormal3d(0, -1, 0);
  glVertex3d(-1, -1, 1);
  glVertex3d(-1, -1, -1);
  glVertex3d(1, -1, -1);
  glVertex3d(1, -1, 1);
  glNormal3d(0, 0, 1);
  glVertex3d(-1, -1, 1);
  glVertex3d(1, -1, 1);
  glVertex3d(1, 1, 1);
  glVertex3d(-1, 1, 1);
  glNormal3d(0, 0, -1);
  glVertex3d(1, -1, -1);
  glVertex3d(-1, -1, -1);
  glVertex3d(-1, 1, -1);
  glVertex3d(1, 1, -1);
  glNormal3d(-1, 0, 0);
  glVertex3d(-1, -1, -1);
  glVertex3d(-1, -1, 1);
  glVertex3d(-1, 1, 1);
  glVertex3d(-1, 1, -1);
  glNormal3d(1, 0, 0);
  glVertex3d(1, -1, 1);
  glVertex3d(1, -1, -1);
  glVertex3d(1, 1, -1);
  glVertex3d(1, 1, 1);
  glEnd();
  glPopMatrix();
  if (!root)
    textQuad(e);
}
GLuint SpatialView::label(const fsv::Entry &e) {
  auto key = e.path.string() + "!" + e.member + dx::font_name +
             std::to_string(dx::size);
  if (auto it = labels.find(key); it != labels.end())
    return it->second;
  if (labels.size() >= 512)
    return 0;
  constexpr int W = 256, H = 128;
  Fl_Image_Surface surface(W, H);
  Fl_Surface_Device::push_current(&surface);
  fl_color(e.directory ? fl_rgb_color(43, 67, 93) : fl_rgb_color(29, 37, 47));
  fl_rectf(0, 0, W, H);
  if (!e.directory && e.member.empty()) {
    auto ext = e.path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif" ||
        ext == ".bmp") {
      auto image = Fl_Shared_Image::get(e.path.c_str());
      if (image && !image->fail()) {
        image->scale(W, H - 30, 1, 1);
        image->draw((W - image->w()) / 2, 0);
      }
      if (image)
        image->release();
    }
  }
  fl_color(FL_FOREGROUND_COLOR);
  fl_font(dx::font, dx::size);
  fl_draw(e.name.c_str(), 5, H - 32, W - 10, 30,
          FL_ALIGN_CENTER | FL_ALIGN_WRAP);
  auto rgb = surface.image();
  Fl_Surface_Device::pop_current();
  if (!rgb)
    return 0;
  std::vector<unsigned char> pixels(W * H * 4, 255);
  auto data = rgb->data()[0];
  int stride = rgb->ld() ? rgb->ld() : W * rgb->d();
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x)
      memcpy(pixels.data() + (y * W + x) * 4, data + y * stride + x * rgb->d(),
             3);
  delete rgb;
  GLuint texture;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               pixels.data());
  labels[key] = texture;
  return texture;
}
void SpatialView::textQuad(const fsv::Entry &e) {
  if (e.extent.x < 1 || e.extent.z < 1)
    return;
  auto texture = label(e);
  if (!texture)
    return;
  auto c = e.center, s = e.extent * .46;
  double y = c.y + e.extent.y / 2 + .04;
  glDisable(GL_LIGHTING);
  glEnable(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, texture);
  glColor4d(1, 1, 1, 1);
  glBegin(GL_QUADS);
  glTexCoord2d(0, 0);
  glVertex3d(c.x - s.x, y, c.z - s.z);
  glTexCoord2d(0, 1);
  glVertex3d(c.x - s.x, y, c.z + s.z);
  glTexCoord2d(1, 1);
  glVertex3d(c.x + s.x, y, c.z + s.z);
  glTexCoord2d(1, 0);
  glVertex3d(c.x + s.x, y, c.z - s.z);
  glEnd();
  glDisable(GL_TEXTURE_2D);
  glEnable(GL_LIGHTING);
}
void SpatialView::draw() {
  if (!context_valid())
    labels.clear();
  auto revision = dx::last + dx::font_name;
  if (theme != revision) {
    invalidateLabels();
    theme = revision;
  }
  unsigned char r, g, b;
  Fl::get_color(FL_BACKGROUND2_COLOR, r, g, b);
  glClearColor(r / 255.f, g / 255.f, b / 255.f, 1);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  setup();
  glEnable(GL_DEPTH_TEST);
  glEnable(GL_NORMALIZE);
  glEnable(GL_COLOR_MATERIAL);
  glEnable(GL_LIGHTING);
  glEnable(GL_LIGHT0);
  GLfloat position[] = {-.5f, 1, .8f, 0}, ambient[] = {.45f, .45f, .5f, 1};
  glLightfv(GL_LIGHT0, GL_POSITION, position);
  glLightfv(GL_LIGHT0, GL_AMBIENT, ambient);
  if (!scene)
    return;
  if (scene->root.extent.x <= 0) {
    fsv::Entry base;
    base.center = {0, -1, 0};
    base.extent = {172, 2, 132};
    glColor3f(.22, .27, .32);
    cube(base, true);
  } else {
    glColor3f(.23, .37, .44);
    cube(scene->root, true);
    glDisable(GL_LIGHTING);
    glColor3f(.68, .31, .25);
    glLineWidth(3);
    glBegin(GL_LINES);
    for (auto &line : scene->branches) {
      glVertex3d(line.first.x, line.first.y, line.first.z);
      glVertex3d(line.second.x, line.second.y, line.second.z);
    }
    glEnd();
    glEnable(GL_LIGHTING);
  }
  for (int i = 0; i < int(scene->entries.size()); ++i) {
    auto &e = scene->entries[i];
    if (i == selected)
      glColor3f(.42, .67, 1);
    else if (e.directory)
      glColor3f(.24, .47, .61);
    else {
      auto hash = std::hash<std::string>{}(e.path.extension().string());
      glColor3f(.35 + (hash % 5) * .07, .38 + ((hash >> 3) % 4) * .08,
                .34 + ((hash >> 6) % 6) * .08);
    }
    cube(e);
  }
  if (selected >= 0 && selected < int(scene->entries.size())) {
    auto &e = scene->entries[selected];
    glDisable(GL_LIGHTING);
    glColor3f(1, .85, .3);
    glLineWidth(2);
    glPushMatrix();
    glTranslated(e.center.x, e.center.y, e.center.z);
    glScaled(e.extent.x * .51, e.extent.y * .51, e.extent.z * .51);
    for (int face = 0; face < 2; ++face) {
      double y = face ? 1 : -1;
      glBegin(GL_LINE_LOOP);
      glVertex3d(-1, y, -1);
      glVertex3d(-1, y, 1);
      glVertex3d(1, y, 1);
      glVertex3d(1, y, -1);
      glEnd();
    }
    glBegin(GL_LINES);
    for (int x : {-1, 1})
      for (int z : {-1, 1}) {
        glVertex3d(x, -1, z);
        glVertex3d(x, 1, z);
      }
    glEnd();
    glPopMatrix();
  }
  glDisable(GL_LIGHTING);
  glFlush();
}
int SpatialView::pickAt(int x, int y) {
  if (!scene || !context())
    return -1;
  make_current();
  setup();
  double ox, oy, oz, tx, ty, tz;
  double scale = pixels_per_unit();
  gluUnProject((x - this->x()) * scale, pixel_h() - (y - this->y()) * scale, 0,
               model, projection, viewport, &ox, &oy, &oz);
  gluUnProject((x - this->x()) * scale, pixel_h() - (y - this->y()) * scale, 1,
               model, projection, viewport, &tx, &ty, &tz);
  return fsv::pick(*scene, {ox, oy, oz}, {tx - ox, ty - oy, tz - oz});
}
void SpatialView::focusSelection() {
  if (scene && selected >= 0 && selected < int(scene->entries.size())) {
    auto p = scene->entries[selected].center;
    view.panX = -p.x;
    view.panZ = -p.z;
    view.zoom = std::clamp(100. / std::max(scene->entries[selected].extent.x,
                                           scene->entries[selected].extent.z),
                           .15, 8.);
    redraw();
  }
}
int SpatialView::handle(int e) {
  if (e == FL_PUSH) {
    take_focus();
    downX = Fl::event_x();
    downY = Fl::event_y();
    button = Fl::event_button();
    dragged = false;
    if (button == 1) {
      selected = pickAt(downX, downY);
      if (selectionChanged)
        selectionChanged(selected);
      if (Fl::event_clicks() && selected >= 0 && openRequested)
        openRequested(selected);
    }
    return 1;
  }
  if (e == FL_DRAG) {
    int dx = Fl::event_x() - downX, dy = Fl::event_y() - downY;
    dragged = dragged || std::abs(dx) + std::abs(dy) > 2;
    if (button == 3) {
      view.yaw += dx * .5;
      view.pitch = std::clamp(view.pitch + dy * .4, -35., 65.);
    } else if (button == 2 || (button == 1 && Fl::event_state(FL_SHIFT))) {
      view.panX += dx * .35 / view.zoom;
      view.panZ += dy * .35 / view.zoom;
    }
    downX = Fl::event_x();
    downY = Fl::event_y();
    redraw();
    return 1;
  }
  if (e == FL_RELEASE && button == 3 && !dragged) {
    selected = pickAt(Fl::event_x(), Fl::event_y());
    if (selectionChanged)
      selectionChanged(selected);
    if (contextRequested)
      contextRequested(selected);
    return 1;
  }
  if (e == FL_MOUSEWHEEL) {
    view.zoom =
        std::clamp(view.zoom * std::pow(1.15, -Fl::event_dy()), .15, 8.);
    redraw();
    return 1;
  }
  if (e == FL_FOCUS || e == FL_UNFOCUS)
    return 1;
  if (e == FL_KEYDOWN) {
    if (Fl::event_key() == FL_Enter && openRequested) {
      openRequested(selected);
      return 1;
    }
    if (Fl::event_key() == 'f') {
      focusSelection();
      return 1;
    }
    if (Fl::event_key() == 'r') {
      reset();
      return 1;
    }
  }
  return Fl_Gl_Window::handle(e);
}
bool SpatialView::snapshot(const std::filesystem::path &path) {
  make_current();
  draw();
  std::vector<unsigned char> pixels(size_t(pixel_w()) * pixel_h() * 3);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, pixel_w(), pixel_h(), GL_RGB, GL_UNSIGNED_BYTE,
               pixels.data());
  std::ofstream file(path, std::ios::binary);
  file << "P6\n" << pixel_w() << " " << pixel_h() << "\n255\n";
  for (int y = pixel_h() - 1; y >= 0; --y)
    file.write((char *)pixels.data() + size_t(y) * pixel_w() * 3,
               pixel_w() * 3);
  return bool(file);
}
