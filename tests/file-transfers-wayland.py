#!/usr/bin/env python3
"""Real cross-process Wayland clipboard and file drops on an isolated nested DomeD."""

import json, os, re, select, signal, socket, subprocess, sys, tempfile, time, urllib.request
from pathlib import Path
from urllib.parse import quote
from Xlib import X, display
from Xlib.ext import xtest

signal.pthread_sigmask(signal.SIG_SETMASK, [])
if os.environ.get("FSVR_FILE_WL_BUS") != "1":
    os.execvpe(
        "dbus-run-session",
        ["dbus-run-session", "--", sys.executable, *sys.argv],
        {**os.environ, "FSVR_FILE_WL_BUS": "1"},
    )
root = Path(__file__).resolve().parents[1]
out = root / "build-fltk/file-transfers-wayland"
out.mkdir(parents=True, exist_ok=True)
children = []
buffers = {}
inbox = {}
try:
    with tempfile.TemporaryDirectory(prefix="fsvr-file-wayland-") as tmp:
        os.chmod(tmp, 0o700)
        rd, wr = os.pipe()
        xvfb = subprocess.Popen(
            [
                "Xvfb",
                "-displayfd",
                str(wr),
                "-screen",
                "0",
                "1600x1000x24",
                "-nolisten",
                "tcp",
            ],
            pass_fds=(wr,),
            stderr=subprocess.DEVNULL,
        )
        children.append(xvfb)
        os.close(wr)
        with os.fdopen(rd) as stream:
            disp = ":" + stream.readline().strip()
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            port = s.getsockname()[1]
        base = f"http://127.0.0.1:{port}"
        env = {
            **os.environ,
            "DISPLAY": disp,
            "XDG_RUNTIME_DIR": tmp,
            "XDG_CONFIG_HOME": tmp + "/config",
            "XDG_DATA_HOME": tmp + "/data",
            "XDG_STATE_HOME": tmp + "/state",
            "XDG_CURRENT_DESKTOP": "Dome",
            "DOME_START_XR": "0",
            "DOME_START_IME": "0",
            "DOME_START_TRAY": "0",
            "DOME_LOGIN_SESSION": "0",
            "DOME_SESSION_RESTORE": "0",
            "DOME_RENDERER": "gles",
            "LIBGL_ALWAYS_SOFTWARE": "1",
            "DOME_PREVIEW_PORT": str(port),
            "DBUS_SYSTEM_BUS_ADDRESS": "unix:path=/nonexistent/fsvr-file-system-bus",
        }
        for key in ["WAYLAND_DISPLAY", "WAYLAND_SOCKET", "FLTK_BACKEND"]:
            env.pop(key, None)

        def start(args, label, **kwargs):
            p = subprocess.Popen(
                args, env=env, stderr=(out / (label + ".log")).open("w"), **kwargs
            )
            children.append(p)
            return p

        compositor = start(
            ["/usr/local/bin/dome-compositor", "--backend", "nested"],
            "compositor",
            stdout=(out / "compositor.log").open("a"),
        )

        def pump(timeout=0.02):
            if not buffers:
                time.sleep(timeout)
                return
            ready, _, _ = select.select(list(buffers), [], [], timeout)
            for fd in ready:
                chunk = os.read(fd, 65536)
                if not chunk:
                    continue
                buffers[fd] += chunk
                while b"\n" in buffers[fd]:
                    line, buffers[fd] = buffers[fd].split(b"\n", 1)
                    inbox[fd].append(json.loads(line))

        def wait(fn, seconds=20):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                try:
                    value = fn()
                except (OSError, ValueError, KeyError):
                    value = None
                if value:
                    return value
                assert compositor.poll() is None, "Private compositor exited"
                pump()
            raise AssertionError(
                "Wayland file transfer timed out; see build-fltk/file-transfers-wayland"
            )

        match = wait(
            lambda: re.search(
                r"Dome Wayland socket: (\S+)", (out / "compositor.log").read_text()
            )
        )
        env.update(
            WAYLAND_DISPLAY=match[1], FLTK_BACKEND="wayland", WAYLAND_DEBUG="client"
        )
        xd = display.Display(disp)
        host = wait(
            lambda: next(
                (
                    w
                    for w in xd.screen().root.query_tree().children
                    if str(w.get_wm_name() or "").startswith("Dome")
                ),
                None,
            )
        )
        host.set_input_focus(X.RevertToParent, X.CurrentTime)
        xd.sync()

        def state():
            return json.load(urllib.request.urlopen(base + "/api/state", timeout=2))[
                "scene"
            ]

        def window(name):
            return next((w for w in state()["windows"] if w["title"] == name), None)

        def action(obj):
            request = urllib.request.Request(
                base + "/api/action",
                data=json.dumps(obj).encode(),
                headers={"Content-Type": "application/json"},
            )
            with urllib.request.urlopen(request, timeout=2) as response:
                assert response.status == 200

        def point(w):
            rect = w["desktop"]
            g = host.get_geometry()
            return int(g.x + rect["x"] + rect["w"] * 0.6), int(
                g.y + rect["y"] + rect["h"] * 0.5
            )

        def motion(w):
            x, y = point(w)
            xtest.fake_input(xd, X.MotionNotify, x=x, y=y)
            xd.sync()
            time.sleep(0.15)

        def button(down):
            xtest.fake_input(xd, X.ButtonPress if down else X.ButtonRelease, 1)
            xd.sync()
            time.sleep(0.1)

        def focus(name):
            w = wait(lambda: window(name))
            action({"op": "focus", "id": w["id"]})
            time.sleep(0.1)
            motion(wait(lambda: window(name)))
            button(True)
            button(False)
            wait(lambda: state()["focused"] == w["id"])
            time.sleep(0.1)
            w = wait(lambda: window(name))
            motion(w)
            (out / "last-scene.json").write_text(json.dumps(state(), indent=2))
            return w

        def fixture(name):
            p = start(
                [str(root / "build-fltk/fltk/fsv-clipboard-fixture"), name],
                name,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
            )
            os.set_blocking(p.stdout.fileno(), False)
            buffers[p.stdout.fileno()] = b""
            inbox[p.stdout.fileno()] = []
            wait(lambda: inbox[p.stdout.fileno()])
            assert inbox[p.stdout.fileno()].pop(0)["event"] == "ready"
            wait(lambda: window(name))
            return p

        def send(p, obj):
            p.stdin.write((json.dumps(obj) + "\n").encode())
            p.stdin.flush()

        def answer(p):
            result = wait(lambda: inbox[p.stdout.fileno()]).pop(0)
            assert result.get("ok", True), result
            return result

        def ask(p, obj):
            send(p, obj)
            return answer(p)

        a, b = fixture("FSVR transfer A"), fixture("FSVR transfer B")
        for name, x in [("FSVR transfer A", 100), ("FSVR transfer B", 700)]:
            action({"op": "desktop_move", "id": window(name)["id"], "x": x, "y": 80})
        time.sleep(0.2)
        paths = [tmp + "/český # %\n日本語.txt", tmp + "/folder with spaces"]
        for name in paths:
            Path(name).write_text("Wayland fixture")
        focus("FSVR transfer A")
        ask(a, {"op": "copy", "paths": paths, "cut": True})
        focus("FSVR transfer B")
        result = ask(b, {"op": "paste"})
        assert result["paths"] == paths and result["cut"], result
        # An independent client replaces the owner with a standard URI list.
        uri = "".join("file://" + quote(p, safe="/") + "\r\n" for p in paths).encode()
        publisher = start(
            ["wl-copy", "--foreground", "--type", "text/uri-list"],
            "wl-copy",
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
        )
        publisher.stdin.write(uri)
        publisher.stdin.close()
        time.sleep(0.3)
        result = ask(b, {"op": "paste"})
        assert result["paths"] == paths and not result["cut"], result
        # Genuine pointer grab and COPY-action negotiation between independent clients.
        focus("FSVR transfer A")
        button(True)
        ask(a, {"op": "drag", "paths": paths})
        motion(wait(lambda: window("FSVR transfer B")))
        button(False)
        dropped = wait(
            lambda: next(
                (j for j in inbox[b.stdout.fileno()] if j.get("event") == "drop"), None
            )
        )
        inbox[b.stdout.fileno()].remove(dropped)
        assert dropped["paths"] == paths and not dropped["cut"], dropped
        ask(b, {"op": "quit"})
        b.wait(timeout=5)
        buffers.pop(b.stdout.fileno())
        inbox.pop(b.stdout.fileno())
        # Drop into the actual file manager and verify the resulting files.
        target = Path(tmp) / "destination"
        target.mkdir()
        manager = start(
            [str(root / "build-fltk/fltk/fsvr"), str(target)],
            "fsvr",
            stdout=subprocess.DEVNULL,
        )
        target_window = wait(
            lambda: next(
                (w for w in state()["windows"] if "fsvr (FLTK)" in w["title"]), None
            )
        )
        time.sleep(0.5)
        action({"op": "desktop_move", "id": target_window["id"], "x": 500, "y": 80})
        time.sleep(0.2)
        target_window = next(
            w for w in state()["windows"] if w["id"] == target_window["id"]
        )
        focus("FSVR transfer A")
        button(True)
        ask(a, {"op": "drag", "paths": paths})
        motion(target_window)
        button(False)
        wait(lambda: (target / Path(paths[0]).name).exists())
        assert (target / Path(paths[0]).name).read_text() == "Wayland fixture"
        assert (target / Path(paths[1]).name).exists()
        ask(a, {"op": "quit"})
        xd.close()
        print(
            "PASS real Wayland cross-process clipboard, external wl-copy, COPY-only drag negotiation, and a real drop into FSVR with Unicode/newline filenames"
        )
finally:
    for p in reversed(children):
        if p.poll() is None:
            p.terminate()
            try:
                p.wait(timeout=4)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
