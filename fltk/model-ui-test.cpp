// SPDX-License-Identifier: MIT
#define main fsvr_application_main
#include "main.cpp"
#undef main
#include <gio/gio.h>

#include <iostream>
#include <stdexcept>
static void require(bool okay, const char *message) {
  if (!okay)
    throw std::runtime_error(message);
}
static void wait(const std::function<bool()> &condition, const char *stage = "model UI") {
  auto end = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (!condition()) {
    if (std::chrono::steady_clock::now() >= end) {
      for (auto w = Fl::first_window(); w; w = Fl::next_window(w))
        if (auto preview = dynamic_cast<ModelPreview *>(w))
          for (int i = 0; i < preview->children(); ++i)
            if (auto output = dynamic_cast<Fl_Multiline_Output *>(preview->Fl_Group::child(i)))
              std::cerr << output->value() << '\n';
      throw std::runtime_error(std::string("Timed out waiting for ") + stage);
    }
    Fl::wait(.01);
  }
}
static void settle() {
  for (int i = 0; i < 10; ++i)
    Fl::wait(.01);
}
int main(int argc, char **argv) {
  if (argc > 1 && std::string(argv[1]) == "--model-xr")
    return fsvr_application_main(argc, argv);
  try {
    require(argc == 2, "Expected capture output path");
    char temp[] = "/tmp/fsvr-model-ui-XXXXXX";
    auto root = fs::path(mkdtemp(temp));
    setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
    setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);
    dx::init();
    fl_register_images();
    std::vector<float> vertices = {-.5f, -.5f, 0, .5f, -.5f, 0, 0, .5f, 0};
    char *encoded = g_base64_encode(reinterpret_cast<guchar *>(vertices.data()),
                                    vertices.size() * sizeof(float));
    Json asset = {
        {"asset", {{"version", "2.0"}}},
        {"buffers",
         Json::array(
             {{{"uri",
                std::string("data:application/octet-stream;base64,") + encoded},
               {"byteLength", 36}}})},
        {"bufferViews", Json::array({{{"buffer", 0}, {"byteLength", 36}}})},
        {"accessors", Json::array({{{"bufferView", 0},
                                    {"componentType", 5126},
                                    {"count", 3},
                                    {"type", "VEC3"},
                                    {"min", {-.5, -.5, 0}},
                                    {"max", {.5, .5, 0}}}})},
        {"materials",
         Json::array(
             {{{"pbrMetallicRoughness", {{"baseColorFactor", {1, 0, 0, 1}}}},
               {"extensions", {{"KHR_materials_unlit", Json::object()}}},
               {"doubleSided", true}}})},
        {"meshes",
         Json::array(
             {{{"primitives", Json::array({{{"attributes", {{"POSITION", 0}}},
                                            {"material", 0}}})}}})},
        {"nodes", Json::array({{{"mesh", 0}}})},
        {"scenes", Json::array({{{"nodes", {0}}}})},
        {"scene", 0}};
    g_free(encoded);
    auto path = root / "live.gltf";
    std::ofstream(path) << asset.dump();
    Browser browser;
    browser.load({root, "", false});
    browser.show();
    wait([&] { return browser.idle(); });
    SpatialView *spatial = nullptr;
    std::function<void(Fl_Group &)> find = [&](Fl_Group &group) {
      for (int i = 0; i < group.children(); ++i) {
        auto c = group.child(i);
        if (auto v = dynamic_cast<SpatialView *>(c))
          spatial = v;
        else if (auto g = dynamic_cast<Fl_Group *>(c))
          find(*g);
      }
    };
    find(browser);
    require(spatial, "Browser spatial view missing");
    int selected = -1;
    for (size_t i = 0; i < browser.currentScene().entries.size(); ++i)
      if (browser.currentScene().entries[i].name == "live.gltf")
        selected = i;
    require(selected >= 0, "glTF not present in file browser");
    spatial->selectionChanged(selected);
    browser.command("Live glTF preview");
    settle();
    ModelPreview *preview = nullptr;
    for (auto w = Fl::first_window(); w; w = Fl::next_window(w))
      if (auto p = dynamic_cast<ModelPreview *>(w))
        preview = p;
    require(preview, "Native live preview window missing");
    wait([&] { return bool(preview->modelView().asset()); });
    preview->command("Orbit model");
    settle();
    require(preview->modelView().failure().empty(), "glTF GPU preview failed");
    auto capture = [&](const fs::path &output) {
      preview->modelView().make_current();
      preview->modelView().draw();
      glFinish();
      std::unique_ptr<Fl_RGB_Image> image(
          fl_capture_window(preview, 0, 0, preview->w(), preview->h()));
      require(bool(image), "Cannot capture native model window");
      std::ofstream out(output, std::ios::binary);
      out << "P6\n" << image->w() << ' ' << image->h() << "\n255\n";
      int stride = image->ld() ? image->ld() : image->w() * image->d();
      for (int y = 0; y < image->h(); ++y)
        for (int x = 0; x < image->w(); ++x)
          out.write(image->data()[0] + y * stride + x * image->d(), 3);
    };
    capture(argv[1]);
    auto old = preview->modelView().asset();
    asset["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"] = {0, 0, 1,
                                                                        1};
    std::ofstream(path) << asset.dump();
    wait([&] { return preview->modelView().asset() != old; });
    require(preview->modelView().asset()->materials[0].base[2] == 1,
            "Live dependency reload did not update material");
    settle();
    capture(std::string(argv[1]) + ".reloaded.ppm");
    // A malformed external edit keeps the last usable object while
    // reporting an error; selecting a new file supersedes stale
    // work without blocking events.
    old = preview->modelView().asset();
    std::ofstream(path) << "{ invalid";
    settle();
    settle();
    require(preview->modelView().asset() == old,
            "Malformed edit discarded the usable live preview");
    auto other = root / "new.gltf";
    std::ofstream(other) << asset.dump();
    preview->open(other, false);
    wait([&] {
      return preview->modelView().asset() &&
             preview->modelView().asset()->path == fs::canonical(other);
    });
    browser.command("Refresh");
    browser.command("MapV");
    browser.command("DiscV");
    wait([&] { return browser.idle(); });
    require(preview->modelView().failure().empty(),
            "Preview GPU resources failed after reloading");
    require(preview->modelView().asset() && preview->modelView().asset()->path == fs::canonical(other),
            "Restoring browser selection must preserve an explicitly opened preview");
    if (const char *report = std::getenv("FSVR_MOCK_REPORT")) {
      fs::remove(report);
      wait([&] { return preview->canOpenXR(); }, "XR availability");
      preview->command("Open in XR");
      wait([&] { return fs::exists(report); }, "XR child report");
      std::ifstream input(report);
      Json trace;
      input >> trace;
      require(trace["stereo_frames"].get<int>() >= 80,
              "Open in XR child did not render stereo");
      require(trace["haptics"].get<int>() > 0,
              "Open in XR child did not acquire controller grips");
      std::cout << "PASS preview Open in XR launches managed child, renders "
                   "stereo and accepts Touch grips\n";
    }
    if(dx::dome()) {
      Fl::e_keysym='w';Fl::e_state=FL_META;
      require(preview->handle(FL_SHORTCUT) && !preview->shown(), "Super+W closes the model preview through its hidden universal menu");
    } else preview->hide();
    browser.hide();
    fs::remove_all(root);
    std::cout << "PASS browser selection/menu, native GPU live "
                 "preview, hot reload, malformed edits, stale "
                 "selection and responsive navigation\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
