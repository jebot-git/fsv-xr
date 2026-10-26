// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "network-client.h"
#include "gpu-window.h"
#include <FL/Fl_Secret_Input.H>

class SmbLogin : public dx::GpuWindow {
  Fl_Box explanation{16, 12, 488, 65};
  Fl_Input user{125, 85, 375, 30, "Username:"};
  Fl_Input domain{125, 125, 375, 30, "Domain:"};
  Fl_Secret_Input password{125, 165, 375, 30, "Password:"};
  Fl_Check_Button guest{125, 205, 375, 30, "Connect as guest"};
  Fl_Button cancelButton{260, 250, 110, 32, "Cancel"};
  Fl_Return_Button connectButton{385, 250, 115, 32, "Connect"};
  bool answered = false;
  int requirements = 0;
  void submit(bool okay) {
    if (answered) return;
    answered = true;
    if (okay) response = Json{{"command", "authenticate"}, {"username", user.value()},
      {"domain", domain.value()}, {"password", password.value()}, {"anonymous", bool(guest.value())}};
    else response = Json{{"command", "cancel"}};
    password.value(""); hide();
  }
public:
  std::optional<Json> response;
  explicit SmbLogin(const Json &prompt) : dx::GpuWindow(520, 300, "SMB authentication") {
    end();
    auto message = (prompt.value("retry", false) ? "Authentication failed. Retry: " : "Sign in to: ") +
                   prompt.value("uri", std::string("SMB server"));
    explanation.copy_label(message.c_str());
    explanation.align(FL_ALIGN_WRAP | FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    user.value(prompt.value("username", std::string()).c_str());
    domain.value(prompt.value("domain", std::string()).c_str());
    auto flags = prompt.value("flags", 0);
    requirements = flags;
    if (!(flags & G_ASK_PASSWORD_ANONYMOUS_SUPPORTED)) guest.deactivate();
    if (!(flags & G_ASK_PASSWORD_NEED_USERNAME)) user.deactivate();
    if (!(flags & G_ASK_PASSWORD_NEED_DOMAIN)) domain.deactivate();
    if (!(flags & G_ASK_PASSWORD_NEED_PASSWORD)) password.deactivate();
    guest.callback(+[](Fl_Widget *, void *p) {
      auto s = static_cast<SmbLogin *>(p);
      if (s->guest.value()) { s->user.deactivate(); s->domain.deactivate(); s->password.deactivate(); }
      else {
        if (s->requirements & G_ASK_PASSWORD_NEED_USERNAME) s->user.activate();
        if (s->requirements & G_ASK_PASSWORD_NEED_DOMAIN) s->domain.activate();
        if (s->requirements & G_ASK_PASSWORD_NEED_PASSWORD) s->password.activate();
      }
    }, this);
    cancelButton.callback(+[](Fl_Widget *, void *p) { static_cast<SmbLogin *>(p)->submit(false); }, this);
    connectButton.callback(+[](Fl_Widget *, void *p) { static_cast<SmbLogin *>(p)->submit(true); }, this);
    callback(+[](Fl_Widget *, void *p) { static_cast<SmbLogin *>(p)->submit(false); }, this);
    set_modal(); show(); password.take_focus();
  }
};
