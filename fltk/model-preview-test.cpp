// SPDX-License-Identifier: MIT
#include "model-preview.h"
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
namespace fs = std::filesystem;
static void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
static void wait(const std::function<bool()> &ready) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!ready()) {
    require(std::chrono::steady_clock::now() < deadline, "Preview frame update timed out");
    Fl::wait(.01);
  }
}
class Preview : public ModelPreview {
public:
  unsigned frames = 0;
  unsigned coloredFrames = 0;
  void draw() override {
    ModelPreview::draw();
    ++frames;
    auto &view = modelView();
    double scale = pixels_per_unit();
    unsigned char pixel[4]{};
    GLint previous;
    glGetIntegerv(GL_READ_BUFFER, &previous);
    glReadBuffer(GL_BACK);
    glReadPixels(int((view.x() + view.w()/2) * scale),
                 pixel_h() - int((view.y() + view.h()/2) * scale),
                 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    glReadBuffer(previous);
    coloredFrames += pixel[0]>180 && pixel[1]<70 && pixel[2]<70;
  }
};
int main() try {
  char temporary[] = "/tmp/fsvr-model-preview-XXXXXX";
  require(mkdtemp(temporary), "Temporary fixture failed");
  struct Cleanup { fs::path path; ~Cleanup() { fs::remove_all(path); } } cleanup{temporary};
  fs::path file = fs::path(temporary) / "fixture.gltf";
  const float vertices[] = {-.5f, -.5f, 0, .5f, -.5f, 0, 0, .5f, 0};
  std::ofstream(fs::path(temporary) / "vertices.bin", std::ios::binary)
      .write(reinterpret_cast<const char *>(vertices), sizeof(vertices));
  std::ofstream(file) << R"({"asset":{"version":"2.0"},"buffers":[{"uri":"vertices.bin","byteLength":36}],"bufferViews":[{"buffer":0,"byteLength":36}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[-0.5,-0.5,0],"max":[0.5,0.5,0]}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"material":0}]}],"materials":[{"doubleSided":true,"extensions":{"KHR_materials_unlit":{}},"pbrMetallicRoughness":{"baseColorFactor":[1,0,0,1]}}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";
  dx::init();
  Preview preview;
  preview.open(file);
  wait([&] { return bool(preview.modelView().asset()); });
  auto first = preview.frames;
  wait([&] { return preview.frames > first + 4; });
  require(preview.modelView().failure().empty(), "Model rendering failed");
  require(preview.coloredFrames > 0, "Containing GPU surface lacks model pixels");
  preview.modelView().orbit = preview.modelView().playing = false;
  for (int i=0;i<40;++i) Fl::wait(.01);
  auto idle = preview.frames;
  for (int i=0;i<40;++i) Fl::wait(.01);
  require(preview.frames <= idle + 1, "Static preview should retain its frame");
  auto beforeReset = preview.frames;
  preview.modelView().reset();
  wait([&] { return preview.frames > beforeReset; });
  preview.hide(); preview.clearPassive();
  require(!preview.modelView().asset(), "Hidden passive preview should release its model");
  preview.modelView().orbit = true;
  preview.open(file);
  wait([&] { return bool(preview.modelView().asset()); });
  auto reopened = preview.frames;
  auto previousColored = preview.coloredFrames;
  wait([&] { return preview.frames > reopened + 4; });
  require(preview.coloredFrames > previousColored, "Reopened GPU surface lacks model pixels");
  preview.hide();
  std::cout << "PASS model loading, animation and reset commit the containing window; idle frames and reopening work\n";
} catch(const std::exception &error) {
  std::cerr << error.what() << '\n'; return 1;
}
