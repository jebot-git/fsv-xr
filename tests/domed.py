#!/usr/bin/env python3
"""Opt-in integration test against a built DomeD compositor on private Xvfb.

Usage: python3 tests/domed.py /path/to/DomeD/target/debug/dome-compositor build/fsv-xr
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
    (fixtures / "select-me.txt").write_text("fsv-xr Vulkan thumbnail test\nDomeD Wayland bridge")
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
        with (out / "compositor.log").open("w") as log:
            dome = subprocess.Popen([compositor], env=env, stdout=log, stderr=log)
        def socket_name():
            assert dome.poll() is None, "DomeD exited; see compositor.log"
            match = re.search(r"Dome Wayland socket: (\S+)", (out / "compositor.log").read_text())
            return match[1] if match else None
        name = wait(socket_name)
        client_env = {**env, "WAYLAND_DISPLAY": name, "QT_QPA_PLATFORM": "wayland", "QT_FORCE_STDERR_LOGGING": "1", "WAYLAND_DEBUG": "1"}
        with (out / "client.log").open("w") as log:
            # Match application-menu launch: detect DomeD without --domed.
            client = subprocess.Popen([executable, str(fixtures)], env=client_env, stdout=log, stderr=log)
        def window():
            assert client.poll() is None, "fsv-xr exited; see client.log"
            return next((w for w in state()["windows"] if "fsv-xr" in w["title"]), None)
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
            # Send a hover frame first, as a tracked hand does before pinching.
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
        xd.close()
        print("PASS: Vulkan/DomeD capture; selected-file xdg_popup, separate preview texture, popup hand input, reopening, dismissal, live light/dark theme, RMB context anchors, nested menus, keyboard Properties/Escape, oversized menu arrows/wheel/scrolled action targeting, MMB drag, internal Trash preview/context Restore and cleanup")
    finally:
        for process in (client, dome, xvfb):
            if process is not None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
