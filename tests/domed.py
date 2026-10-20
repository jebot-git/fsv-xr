#!/usr/bin/env python3
"""Opt-in integration test against a built DomeD compositor on private Xvfb.

Usage: python3 tests/domed.py /path/to/DomeD/target/debug/dome-compositor build/fsvr
Starts an isolated compositor/socket, captures its actual client buffer and injects
XR seat packets. Never modifies DomeD, runtime registrations or session settings.
"""
import json
import os
from pathlib import Path
import re
import socket
import shutil
import subprocess
import sys
import tempfile
import time
import threading
import urllib.request
import urllib.parse
import urllib.error
from PIL import Image, ImageChops, ImageStat
from Xlib import X, XK, display
from Xlib.ext import xtest

out = Path("build/domed-probe")
out.mkdir(parents=True, exist_ok=True)
compositor, executable = map(lambda p: str(Path(p).resolve()), sys.argv[1:3])

def wait(fn, seconds=12):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        result = fn()
        if result:
            return result
        time.sleep(.05)
    raise AssertionError("Timed out waiting for DomeD/client")

with tempfile.TemporaryDirectory(prefix="fsv-dome-") as runtime:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    base = f"http://127.0.0.1:{port}"
    env = {**os.environ, "DOME_PREVIEW_PORT": str(port), "XDG_RUNTIME_DIR": runtime,
           "XDG_CONFIG_HOME": runtime + "/config", "XDG_DATA_HOME": runtime + "/data", "RUST_LOG": "warn"}
    xvfb_path = os.environ.get("DOME_XVFB") or shutil.which("Xvfb") or str(Path(compositor).parents[2] / ".local-deps/root/usr/bin/Xvfb")
    rd, wr = os.pipe()
    xvfb_log = (out / "xvfb.log").open("w")
    xvfb = subprocess.Popen([xvfb_path, "-displayfd", str(wr), "-screen", "0", "1600x1000x24", "-nolisten", "tcp"], pass_fds=(wr,), stdout=xvfb_log, stderr=xvfb_log)
    os.close(wr)
    with os.fdopen(rd) as pipe:
        env["DISPLAY"] = ":" + pipe.readline().strip()
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("WAYLAND_SOCKET", None)
    env.pop("QT_QPA_PLATFORM", None)
    env["LIBGL_ALWAYS_SOFTWARE"] = "1"
    config_dir = Path(env["XDG_CONFIG_HOME"]) / "dome"
    config_dir.mkdir(parents=True)
    appearance_path = config_dir / "appearance.json"
    def set_appearance(light, accent, background):
        temp_path = appearance_path.with_suffix(".json.tmp")
        temp_path.write_text(json.dumps({"light": light, "accent": accent, "background": background, "opacity": 220, "hand_resume_ms": 800}))
        temp_path.replace(appearance_path)
    set_appearance(True, "#d33a79", [.15, .2, .25])
    fixtures = Path(runtime) / "fixtures"
    fixtures.mkdir()
    (fixtures / "select-me.txt").write_text("fsvr Vulkan thumbnail test\nDomeD Wayland bridge")
    env["HOME"] = str(fixtures)
    env["XDG_CACHE_HOME"] = runtime + "/cache"
    env["DOME_FILE_MANAGER"] = executable
    env["DOME_FILE_MANAGER_SERVICE"] = "1"
    bus = subprocess.Popen(["dbus-daemon", "--session", "--nofork", "--print-address=1"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    env["DBUS_SESSION_BUS_ADDRESS"] = bus.stdout.readline().strip()
    heartbeat_stop = threading.Event()
    dome = client = None
    def state():
        with urllib.request.urlopen(base + "/api/state", timeout=2) as response:
            return json.load(response)["scene"]
    def packet(events, actions=None):
        data = json.dumps({"events": events, "actions": actions or []}).encode()
        request = urllib.request.Request(base + "/api/xr-input", data=data, headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(request, timeout=2) as response:
            assert response.status == 200
    try:
        # Exercise an upgraded session too: Linux marks an unlinked running
        # executable as "(deleted)" in /proc, but it is still the same compositor.
        running_compositor = Path(runtime) / "dome-compositor"
        shutil.copy2(compositor, running_compositor)
        with (out / "compositor.log").open("w") as log:
            dome = subprocess.Popen([str(running_compositor)], env=env, stdout=log, stderr=log)
        def socket_name():
            assert dome.poll() is None, "DomeD exited; see compositor.log"
            match = re.search(r"Dome Wayland socket: (\S+)", (out / "compositor.log").read_text())
            return match[1] if match else None
        desktop_name = wait(socket_name)
        name = wait(lambda: (m[1] if (m := re.search(r"Dome XR Wayland socket: (\S+)", (out / "compositor.log").read_text())) else None))
        running_compositor.unlink()
        client_env = {**env, "WAYLAND_DISPLAY": name, "QT_QPA_PLATFORM": "wayland", "QT_FORCE_STDERR_LOGGING": "1", "WAYLAND_DEBUG": "1"}
        # Neither named realm nor an inherited socket may bypass DomeD's
        # mouse/keyboard-only path by requesting standalone hand controls.
        for realm_socket in (desktop_name, name):
            result = subprocess.run([executable, "--xr", "--smoke-test"],
                                    env={**client_env, "WAYLAND_DISPLAY": realm_socket},
                                    capture_output=True, text=True, timeout=10)
            assert result.returncode == 2 and "--xr is unavailable inside DomeD" in result.stderr, result.stderr
        with socket.socket(socket.AF_UNIX) as inherited:
            inherited.connect(str(Path(runtime) / name))
            inherited_env = {**client_env, "WAYLAND_SOCKET": str(inherited.fileno())}
            inherited_env.pop("WAYLAND_DISPLAY", None)
            result = subprocess.run([executable, "--xr", "--smoke-test"], env=inherited_env,
                                    pass_fds=(inherited.fileno(),), capture_output=True, text=True, timeout=10)
            assert result.returncode == 2 and "--xr is unavailable inside DomeD" in result.stderr, result.stderr
        with (out / "client.log").open("w") as log:
            # Match application-menu launch: detect DomeD without --domed.
            client = subprocess.Popen([executable, str(fixtures)], env=client_env, stdout=log, stderr=log)
        def window():
            assert client.poll() is None, "fsvr exited; see client.log"
            return next((w for w in state()["windows"] if "fsvr" in w["title"]), None)
        win = wait(window)
        wid = win["id"]
        def frame(surface=None):
            try:
                with urllib.request.urlopen(base + f"/api/xr-frame/{wid if surface is None else surface}", timeout=2) as response:
                    return Image.frombytes("RGBA", (int(response.headers["X-Dome-Width"]), int(response.headers["X-Dome-Height"])), response.read())
            except urllib.error.HTTPError as error:
                if error.code == 409:
                    return None
                raise
        wait(frame)
        time.sleep(1)
        before = wait(frame)
        before.save(out / "01-vulkan-client.png")
        # QML panels follow DomeD's light palette; the XR scene preserves alpha.
        assert min(before.getpixel((before.width - 15, 75))[:3]) > 170, "DomeD light theme not applied to inspector"
        places_width = min(200, before.width * .2)
        inspector_width = min(310, before.width * .28)
        background_pixel = (int(places_width + 12), 75)
        assert before.getpixel(background_pixel)[3] == 0, "XR background lost its alpha"
        def places_median(image):
            # The Places list scrolls to its current location. A single pixel
            # can land on an icon/label when adding entries such as Trash.
            return ImageStat.Stat(image.crop((0, 80, int(places_width), image.height - 20)).convert("RGB")).median
        assert min(places_median(before)) > 170, "Places did not follow the light theme"
        # One object covers the center of the view between Places and inspector.
        x, y = places_width + (before.width - places_width - inspector_width) / 2, before.height * .56
        uv = [x / before.width, y / before.height]
        def click(button):
            # Send pointer motion before clicking.
            # It also releases any stale pointer grab after a popup unmaps.
            packet([{"kind": "motion", "id": wid, "uv": uv}], [{"op": "focus", "id": wid}])
            time.sleep(.08)
            packet([{"kind": "motion", "id": wid, "uv": uv},
                    {"kind": "button", "button": button, "pressed": True},
                    {"kind": "button", "button": button, "pressed": False}], [{"op": "focus", "id": wid}])
        click(0x110)
        time.sleep(.3)
        selected = wait(frame)
        assert ImageChops.difference(before.convert("RGB"), selected.convert("RGB")).getbbox(), "LMB selection did not update client content"
        selected.save(out / "02-selected.png")
        popup = wait(lambda: next((p for p in state().get("popups", []) if p["root"] == wid), None))
        assert popup["parent"] == wid and popup["id"] >= (1 << 48), popup
        assert len(state()["windows"]) == 1, "Preview consumed a normal window/lane slot"
        assert 0 <= popup["anchor"]["x"] <= before.width and 0 <= popup["anchor"]["y"] <= before.height, popup
        light_popup = wait(lambda: frame(popup["id"]))
        light_popup.save(out / "03-file-preview.png")
        assert min(light_popup.getpixel((5,5))[:3]) > 180, "Preview does not use DomeD light palette"
        assert abs(light_popup.getpixel((5,5))[3] - 220) <= 1, "Preview does not use DomeD panel opacity"
        set_appearance(False, "#53ad7f", [.02, .03, .04])
        def dark_popup_ready():
            image = frame(popup["id"])
            return image if image and max(image.getpixel((5,5))[:3]) < 65 else None
        wait(dark_popup_ready).save(out / "03b-live-dark-preview.png")
        def dark_main_ready():
            image = frame()
            return image if image and max(places_median(image)) < 70 and max(image.getpixel((5,5))[:3]) < 70 and image.getpixel(background_pixel)[3] == 0 else None
        wait(dark_main_ready).save(out / "03c-live-dark-main.png")
        def popup_click(surface, uv):
            packet([{"kind": "motion", "id": surface, "uv": uv},
                    {"kind": "button", "button": 0x110, "pressed": True},
                    {"kind": "button", "button": 0x110, "pressed": False}])
        # Close is the rightmost button. This targets the real popup texture's UVs.
        popup_click(popup["id"], [.85, .94])
        wait(lambda: not state().get("popups", []))
        time.sleep(.4) # DomeD suspends hit targets during popup focus transitions.
        # Selecting the same entry again reopens a dismissed preview.
        click(0x110)
        reopened = wait(lambda: next((p for p in state().get("popups", []) if p["root"] == wid), None))
        wait(lambda: frame(reopened["id"])).save(out / "04-reopened-preview.png")
        # RMB on the file preview creates a child context surface, associated with
        # the exact popup-local point rather than an invented client positioner.
        packet([{"kind": "motion", "id": reopened["id"], "uv": [.5, .4]},
                {"kind": "button", "button": 0x111, "pressed": True},
                {"kind": "button", "button": 0x111, "pressed": False}])
        child_menu = wait(lambda: next((p for p in state().get("popups", []) if p.get("context_at") is not None), None))
        assert child_menu["parent"] == reopened["id"], child_menu
        assert abs(child_menu["context_at"][0] - 170) <= 1, child_menu
        assert abs(child_menu["context_at"][1] - reopened["anchor"]["h"] * .4) <= 1, child_menu
        wait(lambda: frame(child_menu["id"])).save(out / "05-preview-context.png")
        # Native Qt menus own keyboard navigation. End + Return selects Properties.
        xd = display.Display(env["DISPLAY"])
        host = wait(lambda: next((w for w in xd.screen().root.query_tree().children if str(w.get_wm_name() or "").startswith("Dome")), None))
        host.set_input_focus(X.RevertToParent, X.CurrentTime); xd.sync()
        def key(name):
            code = xd.keysym_to_keycode(XK.string_to_keysym(name))
            for kind in (X.KeyPress, X.KeyRelease):
                xtest.fake_input(xd, kind, code); xd.sync(); time.sleep(.03)
        key("Escape")
        wait(lambda: not any(p.get("context_at") is not None for p in state().get("popups", [])))
        # A single RMB on the file must work even while its preview owns a grab.
        click(0x111)
        menu = wait(lambda: next((p for p in state().get("popups", []) if p.get("context_at") is not None), None))
        assert menu["root"] == wid and menu["parent"] == wid, menu
        assert all(abs(a-b) < 1 for a,b in zip(menu["context_at"], (x,y))), menu
        assert len(state()["windows"]) == 1
        wait(lambda: frame(menu["id"])).save(out / "05b-file-context.png")
        key("End"); key("Return")
        properties = wait(lambda: next((p for p in state().get("popups", []) if p.get("context_at") is None), None))
        assert properties["parent"] == wid, properties
        popup_click(properties["id"], [.85,.94])
        wait(lambda: not state().get("popups", []))
        click(0x110)
        wait(lambda: state().get("popups", []))
        # Outside-click dismissal still works after nested/native context menus.
        packet([{"kind": "motion", "id": wid, "uv": [.1,.8]},
                {"kind": "button", "button": 0x110, "pressed": True},
                {"kind": "button", "button": 0x110, "pressed": False}])
        wait(lambda: not state().get("popups", []))
        time.sleep(.3)
        start = wait(frame)
        packet([{"kind": "motion", "id": wid, "uv": uv}])
        time.sleep(.08)
        packet([{"kind": "motion", "id": wid, "uv": uv},
                {"kind": "button", "button": 0x112, "pressed": True},
                {"kind": "motion", "id": wid, "uv": [uv[0] + .08, uv[1] + .04]},
                {"kind": "button", "button": 0x112, "pressed": False}])
        time.sleep(.3)
        dragged = wait(frame)
        assert ImageChops.difference(start.convert("RGB"), dragged.convert("RGB")).getbbox(), "MMB drag did not update view"
        dragged.save(out / "06-mmb-drag.png")
        client.terminate()
        client.wait(timeout=3)
        wait(lambda: not state()["windows"] and not state().get("popups", []))
        # Large accessibility fonts make this real menu taller than DomeD's
        # work area, exercising its viewport without adding synthetic actions.
        menu_style = Path(runtime) / "large-menu.qss"
        menu_style.write_text("QMenu { font-size: 40pt; }")
        # FD-based Wayland activation must also select XR presentation without
        # an explicit mode or platform flag, even with an inherited X11 DISPLAY.
        with socket.socket(socket.AF_UNIX) as inherited, (out / "large-menu-client.log").open("w") as log:
            inherited.connect(str(Path(runtime) / name))
            fd_env = {**client_env, "WAYLAND_SOCKET": str(inherited.fileno())}
            fd_env.pop("QT_QPA_PLATFORM", None)
            fd_env.pop("WAYLAND_DISPLAY", None)
            client = subprocess.Popen([executable, "-stylesheet", str(menu_style), str(fixtures)],
                env=fd_env, pass_fds=(inherited.fileno(),), stdout=log, stderr=log)
        wid = wait(window)["id"]
        wait(frame); time.sleep(.5)
        image = wait(frame)
        image.save(out / "07-large-font-main.png")
        x, y = places_width + (image.width - places_width - inspector_width) / 2, image.height * .56
        uv = [x / image.width, y / image.height]
        click(0x111)
        long_menu = wait(lambda: next((p for p in state().get("popups", []) if p.get("context_at") is not None), None))
        viewport = long_menu["viewport"]
        assert viewport["content_height"] > viewport["height"], long_menu
        wait(lambda: frame(long_menu["id"])).save(out / "07-long-context-top.png")
        def menu_state():
            return next(p for p in state()["popups"] if p["id"] == long_menu["id"])
        popup_click(long_menu["id"], [.5, (viewport["height"] - 12) / viewport["height"]])
        wait(lambda: menu_state()["viewport"]["offset"] > 0)
        first_offset = menu_state()["viewport"]["offset"]
        packet([{"kind": "motion", "id": long_menu["id"], "uv": [.5,.5]},
                {"kind": "scroll", "horizontal": 0, "vertical": 120}])
        wait(lambda: menu_state()["viewport"]["offset"] > first_offset)
        scrolled = menu_state()["viewport"]["offset"]
        popup_click(long_menu["id"], [.5, 12 / viewport["height"]])
        wait(lambda: menu_state()["viewport"]["offset"] < scrolled)
        packet([{"kind": "motion", "id": long_menu["id"], "uv": [.5,.5]},
                {"kind": "scroll", "horizontal": 0, "vertical": 1000}])
        time.sleep(.15)
        wait(lambda: frame(long_menu["id"])).save(out / "08-long-context-bottom.png")
        popup_click(long_menu["id"], [.5, (viewport["height"] - 24 - 20) / viewport["height"]])
        wait(lambda: any(p.get("context_at") is None for p in state()["popups"]))
        key("Escape")
        wait(lambda: not state().get("popups", []))
        client.terminate(); client.wait(timeout=3)
        wait(lambda: not state()["windows"] and not state().get("popups", []))
        # Internal Trash uses the same spatial preview and a specialised native menu.
        trash = Path(env["XDG_DATA_HOME"]) / "Trash"
        (trash / "files").mkdir(parents=True)
        (trash / "info").mkdir()
        original = Path(runtime) / "restored document.txt"
        trashed = trash / "files" / "dome-trash-test"
        trashed.write_text("Spatial Trash preview fixture")
        metadata = trash / "info" / "dome-trash-test.trashinfo"
        metadata.write_text("[Trash Info]\nPath=" + urllib.parse.quote(str(original)) + "\nDeletionDate=2026-10-19T12:34:56\n")
        with (out / "trash-client.log").open("w") as log:
            client = subprocess.Popen([executable, "trash:/"], env=client_env, stdout=log, stderr=log)
        wid = wait(window)["id"]
        image = wait(frame); time.sleep(.6)
        uv = [(places_width + (image.width - places_width - inspector_width)/2) / image.width, .56]
        click(0x110)
        trash_preview = wait(lambda: next((p for p in state().get("popups", []) if p["root"] == wid), None))
        wait(lambda: frame(trash_preview["id"])).save(out / "09-trash-preview.png")
        click(0x111)
        trash_menu = wait(lambda: next((p for p in state().get("popups", []) if p.get("context_at") is not None), None))
        wait(lambda: frame(trash_menu["id"])).save(out / "10-trash-context.png")
        key("Home"); key("Return") # First action is Restore, not Open.
        wait(lambda: original.exists())
        assert original.read_text() == "Spatial Trash preview fixture"
        assert not trashed.exists() and not metadata.exists(), "Restored item or metadata remained in Trash"
        client.terminate(); client.wait(timeout=3)
        wait(lambda: not state()["windows"] and not state().get("popups", []))
        def post(path, value):
            request = urllib.request.Request(base + path, data=json.dumps(value).encode(), headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(request, timeout=3) as response:
                return json.load(response)
        def control():
            with urllib.request.urlopen(base + "/api/xr-control", timeout=2) as response:
                return json.load(response)
        def heartbeat():
            while not heartbeat_stop.is_set():
                post("/api/xr-session", {"visible": True, "focused": True, "head": {"position_m": [0,1.4,0], "orientation_xyzw": [0,0,0,1]}, "horizontal_fov_deg": 100})
                heartbeat_stop.wait(.1)
        pulse = threading.Thread(target=heartbeat, daemon=True); pulse.start()
        wait(lambda: control()["visible"] and control()["input_realm"] == "xr")
        # Desktop auto-detection also works through an inherited FD while XR is active.
        with socket.socket(socket.AF_UNIX) as inherited, (out / "desktop-realm.log").open("w") as log:
            inherited.connect(str(Path(runtime) / desktop_name))
            desktop_env = {**client_env, "WAYLAND_SOCKET": str(inherited.fileno())}
            desktop_env.pop("WAYLAND_DISPLAY", None)
            client = subprocess.Popen([executable, str(fixtures)], env=desktop_env, pass_fds=(inherited.fileno(),), stdout=log, stderr=log)
        win = wait(window); wid = win["id"]
        assert win["origin"] == "desktop" and win["desktop_visible"], win
        desktop_frame = wait(frame); time.sleep(.4); desktop_frame = wait(frame)
        assert desktop_frame.getpixel(background_pixel)[3] == 255, "Desktop realm incorrectly uses XR transparency"
        desktop_frame.save(out / "11-desktop-realm.png")
        client.terminate(); client.wait(timeout=3)
        wait(lambda: not state()["windows"])
        # Query session-scoped MIME defaults and exercise the real FileManager1 service.
        applications = Path(env["XDG_DATA_HOME"]) / "applications"; applications.mkdir(exist_ok=True)
        desktop_entry = Path(__file__).resolve().parents[1] / "data/org.fsvxr.fsvr.desktop"
        (applications / desktop_entry.name).write_text(desktop_entry.read_text().replace("Exec=fsvr", "Exec=" + executable).replace("TryExec=fsvr", "TryExec=" + executable))
        subprocess.run([sys.executable, str(Path(__file__).resolve().parents[1] / "scripts/set-dome-default.py"), "--config-home", env["XDG_CONFIG_HOME"]], check=True, capture_output=True)
        query = subprocess.run(["gio", "mime", "inode/directory"], env={**client_env, "XDG_CURRENT_DESKTOP": "Dome"}, check=True, capture_output=True, text=True)
        assert "org.fsvxr.fsvr.desktop" in query.stdout.splitlines()[0], query.stdout
        def manager(method, location, realm):
            request_env = {**client_env, "WAYLAND_DISPLAY": desktop_name if realm == "desktop" else name}
            subprocess.run(["gdbus", "call", "--session", "--dest", "org.freedesktop.FileManager1", "--object-path", "/org/freedesktop/FileManager1", "--method", "org.freedesktop.FileManager1." + method, "['" + location + "']", ""], env=request_env, check=True, capture_output=True, timeout=5)
            return wait(lambda: next((w for w in state()["windows"] if "fsvr" in w["title"]), None))
        def close_window():
            post("/api/action", {"op": "close", "id": wid})
            wait(lambda: not state()["windows"] and not state().get("popups", []))
        for realm, realm_socket in [("desktop", desktop_name), ("xr", name)]:
            with (out / ("gio-" + realm + ".log")).open("w") as log:
                subprocess.run(["gio", "open", fixtures.as_uri()], env={**client_env, "WAYLAND_DISPLAY": realm_socket, "XDG_CURRENT_DESKTOP": "Dome"}, check=True, stdout=log, stderr=log, timeout=5)
            win = wait(lambda: next((w for w in state()["windows"] if "fsvr" in w["title"]), None)); wid = win["id"]
            assert win["origin"] == realm, "GIO default folder activation lost its launch realm"
            opened = wait(frame); time.sleep(.2); opened = wait(frame)
            assert opened.getpixel(background_pixel)[3] == (255 if realm == "desktop" else 0)
            close_window()
        win = manager("ShowItems", (fixtures / "select-me.txt").as_uri(), "desktop"); wid = win["id"]
        assert win["origin"] == "desktop", "FileManager1 ignored the background desktop caller's realm"
        wait(frame); time.sleep(.4)
        assert not state().get("popups", []), "Desktop reveal opened a spatial popup"
        close_window()
        win = manager("ShowItemProperties", (fixtures / "select-me.txt").as_uri(), "xr"); wid = win["id"]
        assert win["origin"] == "xr" and not win["desktop_visible"], win
        property_popup = wait(lambda: next((p for p in state().get("popups", []) if p["root"] == wid), None))
        wait(lambda: frame(property_popup["id"])).save(out / "12-filemanager-properties.png")
        popup_click(property_popup["id"], [.85,.94])
        wait(lambda: not state().get("popups", [])); close_window()
        # Super+E follows the active XR realm and uses the configured default manager.
        modifier = xd.keysym_to_keycode(XK.string_to_keysym("Super_L"))
        xtest.fake_input(xd, X.KeyPress, modifier); xd.sync(); key("e")
        xtest.fake_input(xd, X.KeyRelease, modifier); xd.sync()
        win = wait(lambda: next((w for w in state()["windows"] if "fsvr" in w["title"]), None)); wid = win["id"]
        assert win["origin"] == "xr" and not win["desktop_visible"], win
        wait(frame); time.sleep(.7)
        image = wait(frame)
        uv = [(places_width + (image.width - places_width - inspector_width)/2) / image.width, .56]
        physical = [450, 400]
        def motion(x, y):
            geo = host.get_geometry()
            physical[:] = [x, y]
            xtest.fake_input(xd, X.MotionNotify, x=int(x+geo.x), y=int(y+geo.y)); xd.sync(); time.sleep(.08)
        def button(number, pressed=None):
            for down in ([True, False] if pressed is None else [pressed]):
                xtest.fake_input(xd, X.ButtonPress if down else X.ButtonRelease, number); xd.sync(); time.sleep(.05)
        motion(*physical)
        def aim(target=uv):
            wait(lambda: not control()["mouse"])
            packet([{"kind": "motion", "id": wid, "uv": target}], [{"op": "focus", "id": wid}]); time.sleep(.08)
            motion(physical[0]+1, physical[1])
            wait(lambda: control()["mouse"] and control().get("cursor") and control()["cursor"]["id"] == wid)
        def view_crop(image):
            return image.crop((int(places_width+5), 100, image.width-int(inspector_width)-5, image.height-45))
        def changed(before, message):
            time.sleep(.2)
            after = wait(frame)
            assert ImageChops.difference(view_crop(before).convert("RGB"), view_crop(after).convert("RGB")).getbbox(), message
            return after
        aim(); button(1)
        physical_popup = wait(lambda: next((p for p in state().get("popups", []) if p["root"] == wid), None))
        wait(lambda: frame(physical_popup["id"])).save(out / "13-physical-mouse-preview.png")
        key("Escape"); wait(lambda: not state().get("popups", []))
        aim(); button(3)
        wait(lambda: any(p.get("context_at") is not None for p in state().get("popups", [])))
        key("Escape"); wait(lambda: not state().get("popups", []))
        aim(); before = wait(frame); button(2, True); motion(physical[0]+30, physical[1]+15)
        time.sleep(.85)
        assert control()["mouse"], "XR MMB drag lost ownership while held"
        button(2, False); changed(before, "Physical XR MMB drag did not pan the view")
        aim(); before = wait(frame); button(2); motion(physical[0], physical[1]-28)
        changed(before, "XR scroll-button zoom did not receive hover motion")
        button(2) # Stop scroll-button mode.
        aim(); before = wait(frame); button(4)
        changed(before, "Physical XR wheel did not zoom")
        aim([(places_width+12)/image.width, .23]); before = wait(frame)
        button(1, True); motion(physical[0]+30, physical[1]+22); button(1, False)
        changed(before, "Physical XR LMB background drag did not tilt")
        close_window()
        # Preview delay must leave real mouse double-clicks usable for opening folders.
        double = Path(runtime) / "double"; (double / "child").mkdir(parents=True)
        win = manager("ShowFolders", double.as_uri(), "xr"); wid = win["id"]
        wait(frame); time.sleep(.5); aim()
        button(1); button(1)
        wait(lambda: any(w["id"] == wid and w["title"].endswith("/double/child") for w in state()["windows"]))
        assert not state().get("popups", []), "Preview stole the double-click pointer grab"
        close_window()
        heartbeat_stop.set(); pulse.join(timeout=2)
        xd.close()
        print("PASS: Vulkan/DomeD capture; selected-file xdg_popup, separate preview texture, popup pointer input, reopening, dismissal, live light/dark theme, RMB context anchors, nested menus, keyboard Properties/Escape, oversized menu arrows/wheel/scrolled action targeting, MMB drag, internal Trash preview/context Restore, desktop/XR named and FD launch realms, MIME defaults, FileManager1 reveal/properties, Super+E, physical XR LMB/RMB/MMB/wheel/scroll-button/double-click and cleanup")
    finally:
        heartbeat_stop.set()
        if dome is not None and dome.poll() is None:
            try:
                for win in state()["windows"]:
                    request = urllib.request.Request(base + "/api/action", data=json.dumps({"op":"close", "id":win["id"]}).encode(), headers={"Content-Type":"application/json"})
                    urllib.request.urlopen(request, timeout=2).close()
                time.sleep(.3)
            except Exception:
                pass
        for process in (client, dome, xvfb, bus):
            if process is not None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
