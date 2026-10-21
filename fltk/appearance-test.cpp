// SPDX-License-Identifier: MIT
#include "appearance.h"
#include "gpu-window.h"
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Value_Input.H>
#include <FL/fl_ask.H>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <chrono>
#include <stdexcept>
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
int main(int argc, char **argv) try {
  if (argc != 2)
    return 2;
  char name[] = "/tmp/de-xr-fltk-font-XXXXXX";
  auto dir = mkdtemp(name);
  require(dir, "Temporary configuration");
  setenv("XDG_CONFIG_HOME", dir, 1);
  setenv("XDG_CURRENT_DESKTOP", "Dome", 1);
  std::filesystem::create_directories(dx::config().parent_path());
  require(FcConfigAppFontAddFile(nullptr, (FcChar8 *)argv[1]),
          "Load Cozette fixture");
  FcConfigBuildFonts(nullptr);
  auto write = [](const char *data) {
    auto temp = dx::config();
    temp += ".tmp";
    std::ofstream(temp) << data;
    std::filesystem::rename(temp, dx::config());
  };
  write(
      R"({"light":false,"font":"Cozette / Medium","font_size":13,"accent":"#123456"})");
  dx::init();
  Fl_Double_Window window(400, 160);
  Fl_Input input(10, 10, 380, 40);
  Fl_Value_Input value(10, 70, 380, 40);
  window.end();
  window.show();
  dx::appearance();
  dx::style(&window);
  Fl::check();
  require(dx::font == FL_FREE_FONT && dx::size == 13,
          "Select the native bitmap strike");
  require(input.textfont() == FL_FREE_FONT && input.textsize() == 13,
          "Input inherits bitmap font");
  require(value.textfont() == FL_FREE_FONT && value.textsize() == 13,
          "Numeric inputs inherit bitmap font");
  require(!Fl::option(Fl::OPTION_FNFC_USES_GTK) &&
              !Fl::option(Fl::OPTION_FNFC_USES_KDIALOG) &&
              !Fl::option(Fl::OPTION_FNFC_USES_ZENITY),
          "Native FLTK choosers require no external GUI toolkit");
  require(fl_message_font_ == dx::font && fl_message_size_ == dx::size &&
              FL_NORMAL_SIZE == dx::size,
          "Standard dialog defaults follow the session font");
  struct ModalCheck {
    bool inspected = false;
    std::string failure;
  } modal;
  Fl::add_timeout(
      .1,
      [](void *data) {
        auto &state = *(ModalCheck *)data;
        try {
          auto w = Fl::modal();
          require(w, "Standard input dialog is modal");
          std::function<void(Fl_Widget *)> inspect = [&](Fl_Widget *item) {
            require(
                item->labelfont() == dx::font && item->labelsize() == dx::size,
                "New dialog labels inherit bitmap appearance on first frame");
            if (auto field = dynamic_cast<Fl_Input_ *>(item))
              require(field->textfont() == dx::font &&
                          field->textsize() == dx::size,
                      "Standard input text inherits the native bitmap strike");
            if (auto group = dynamic_cast<Fl_Group *>(item))
              for (int i = 0; i < group->children(); ++i)
                inspect(group->child(i));
          };
          inspect(w);
          state.inspected = true;
          w->do_callback();
        } catch (const std::exception &e) {
          state.failure = e.what();
          if (auto w = Fl::modal())
            w->hide();
        }
      },
      &modal);
  fl_input("Appearance dialog fixture", "Bitmap input");
  require(modal.failure.empty(), modal.failure.c_str());
  require(modal.inspected, "Input dialog appearance inspected");
  {
    Fl_Native_File_Chooser chooser;
    chooser.title("Native FLTK chooser fixture");
    chooser.directory(dir);
    Fl::add_timeout(
        .1,
        [](void *data) {
          auto &state = *(ModalCheck *)data;
          try {
            auto w = Fl::modal();
            require(w, "Native FLTK chooser opens in the application");
            require(w->labelfont() == dx::font && w->labelsize() == dx::size,
                    "File chooser inherits session bitmap labels");
            bool found = false;
            std::function<void(Fl_Widget *)> inspect = [&](Fl_Widget *item) {
              if (auto field = dynamic_cast<Fl_Input_ *>(item)) {
                found = true;
                require(field->textfont() == dx::font &&
                            field->textsize() == dx::size,
                        "File chooser fields inherit session text style");
              }
              if (auto group = dynamic_cast<Fl_Group *>(item))
                for (int i = 0; i < group->children(); ++i)
                  inspect(group->child(i));
            };
            inspect(w);
            require(found, "File chooser input fields present");
            w->do_callback();
          } catch (const std::exception &e) {
            state.failure = e.what();
            if (auto w = Fl::modal())
              w->hide();
          }
        },
        &modal);
    chooser.show();
    require(modal.failure.empty(), modal.failure.c_str());
  }
  FT_Library library;
  FT_Face face;
  require(!FT_Init_FreeType(&library) &&
              !FT_New_Face(library, argv[1], 0, &face),
          "FreeType reference");
  FT_Select_Size(face, 0);
  FT_Load_Char(face, 'M', FT_LOAD_DEFAULT);
  double expected = face->glyph->advance.x / 64.;
  fl_font(dx::font, dx::size);
  require(fl_width("MMMM", 4) == expected * 4,
          "FLTK text metrics use real fixed-strike advances rather than Pango "
          "fallback");
  struct GLFontWindow : dx::GpuWindow {
    double measured = 0;
    GLFontWindow() : dx::GpuWindow(400, 160, "GPU bitmap font fixture") {
      end();
    }
    void draw() override {
      dx::GpuWindow::draw();
      draw_begin();
      fl_font(dx::font, dx::size);
      measured = fl_width("MMMM", 4);
      fl_color(FL_FOREGROUND_COLOR);
      fl_draw("MMMM", 10, 40);
      draw_end();
    }
  } gl;
  gl.show();
  for (int i = 0; i < 4; ++i)
    Fl::wait(.05);
  require(gl.measured == expected * 4,
          "GPU widget text uses native bitmap advances");
  gl.hide();
  FT_Done_Face(face);
  FT_Done_FreeType(library);
  write(
      R"({"light":true,"font":"Cozette / Medium","font_size":13,"accent":"#654321"})");
  for (auto end =
           std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
       std::chrono::steady_clock::now() < end;)
    Fl::wait(.05);
  unsigned char r, g, b;
  Fl::get_color(FL_BACKGROUND_COLOR, r, g, b);
  require(r == 229 && g == 233 && b == 239,
          "Atomic appearance reload updates live palette");
  write("{broken");
  for (auto end =
           std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
       std::chrono::steady_clock::now() < end;)
    Fl::wait(.05);
  Fl::get_color(FL_BACKGROUND_COLOR, r, g, b);
  require(r == 229 && g == 233 && b == 239,
          "Malformed appearance retains previous palette");
  write(R"({"light":false,"font":"Cozette / Medium","font_size":14})");
  dx::appearance();
  require(dx::font == FL_HELVETICA && dx::size == 14,
          "Unavailable bitmap strike falls back to native toolkit font");
  window.hide();
  std::filesystem::remove_all(dir);
  puts("PASS: native bitmap metrics, editable controls, atomic live "
       "appearance, malformed snapshot and unavailable-strike fallback");
  return 0;
} catch (const std::exception &e) {
  fprintf(stderr, "%s\n", e.what());
  return 1;
}
