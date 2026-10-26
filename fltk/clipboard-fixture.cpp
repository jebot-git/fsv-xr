// SPDX-License-Identifier: LGPL-2.1-or-later
#include "file-clipboard.h"
#include <FL/Fl_Box.H>
#include <FL/Fl_Double_Window.H>
#include <iostream>
#include <nlohmann/json.hpp>
using Json = nlohmann::json;
Json encode(const fsv::FileList &f) {
  std::vector<std::string> paths;
  for (auto &p : f.paths)
    paths.push_back(p.string());
  return {{"paths", paths}, {"cut", f.cut}};
}
fsv::FileList decode(const Json &j) {
  fsv::FileList f;
  for (auto &p : j.at("paths"))
    f.paths.emplace_back(p.get<std::string>());
  f.cut = j.value("cut", false);
  return f;
}
struct Fixture {
  struct Window : Fl_Double_Window {
    using Fl_Double_Window::Fl_Double_Window;
    FileClipboard *files = nullptr;
    int handle(int e) override {
      if (files) {
        int result = files->handle(e);
        if (result >= 0)
          return result;
      }
      if (e == FL_PUSH || e == FL_DRAG || e == FL_RELEASE)
        return 1;
      return Fl_Double_Window::handle(e);
    }
  };
  Window window{360, 240, "FSVR file transfer fixture"};
  Fl_Box label{20, 20, 320, 200, "File clipboard / drop fixture"};
  FileClipboard clipboard{window};
  bool running = true;
  std::future<fsv::FileList> pending;
  Fixture() {
    window.end();
    window.files = &clipboard;
    clipboard.acceptsDrop = [](int, int) { return true; };
    clipboard.dropped = [](fsv::FileList files, int, int) {
      auto j = encode(files);
      j["event"] = "drop";
      std::cout << j << std::endl;
    };
    clipboard.failed = [](const std::string &e) {
      std::cout << Json{{"event", "error"}, {"error", e}} << std::endl;
    };
  }
  void command() {
    std::string line;
    if (!std::getline(std::cin, line)) {
      running = false;
      return;
    }
    try {
      auto request = Json::parse(line);
      auto op = request.at("op").get<std::string>();
      if (op == "copy")
        clipboard.copy(decode(request));
      else if (op == "paste") {
        pending = clipboard.paste();
        return;
      } else if (op == "drag")
        clipboard.drag(decode(request));
      else if (op == "quit")
        running = false;
      else
        throw std::runtime_error("Unknown fixture command");
      std::cout << Json{{"ok", true}} << std::endl;
    } catch (const std::exception &e) {
      std::cout << Json{{"ok", false}, {"error", e.what()}} << std::endl;
    }
  }
};
int main(int argc, char **argv) {
  try {
    Fixture fixture;
    if (argc > 1)
      fixture.window.copy_label(argv[1]);
    fixture.window.show();
    Fl::add_fd(0, FL_READ, [](int, void *p) { static_cast<Fixture *>(p)->command(); }, &fixture);
    std::cout << Json{{"event", "ready"}} << std::endl;
    while (fixture.running) {
      Fl::wait(.01);
      fixture.clipboard.poll();
      if (fixture.pending.valid() &&
          fixture.pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
          auto result = encode(fixture.pending.get());
          result["ok"] = true;
          std::cout << result << std::endl;
        } catch (const std::exception &e) {
          std::cout << Json{{"ok", false}, {"error", e.what()}} << std::endl;
        }
      }
    }
    Fl::remove_fd(0);
    fixture.window.hide();
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
