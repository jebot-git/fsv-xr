// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "desktop-files.h"
#include <FL/Fl_Window.H>
#include <future>
// Native file MIME types on Wayland/X11; no toolkit clipboard conversion or shell helpers.
class FileClipboard {
  struct Impl;
  std::unique_ptr<Impl> impl;

public:
  explicit FileClipboard(Fl_Window &);
  ~FileClipboard();
  void poll();
  // Coordinate FLTK's default Wayland drop completion with raw file MIME reception.
  // Returns -1 for events handled by the normal widget dispatcher.
  int handle(int event);
  static int filter(int event, Fl_Window *);
  void copy(const fsv::FileList &);
  std::future<fsv::FileList> paste();
  void moved(const fsv::FileList &, const std::vector<std::filesystem::path> &);
  bool drag(const fsv::FileList &);
  std::function<bool(int, int)> acceptsDrop;
  std::function<void(fsv::FileList, int, int)> dropped;
  std::function<void(const std::string &)> failed;
};
