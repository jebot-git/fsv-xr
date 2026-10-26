#!/usr/bin/env python3
"""File MIME/INCR/XDND interoperability with an independent Xlib client, on Xvfb."""

import json, os, select, signal, subprocess, sys, tempfile, time
from pathlib import Path
from urllib.parse import quote
from Xlib import X, Xatom, display, protocol
from Xlib.ext import xtest

signal.pthread_sigmask(signal.SIG_SETMASK, [])
root = Path(__file__).resolve().parents[1]
children = []
buffers = {}
inbox = {}
events = []
with tempfile.TemporaryDirectory(prefix="fsvr-file-wire-") as directory:
    try:
        rd, wr = os.pipe()
        server = subprocess.Popen(
            [
                "Xvfb",
                "-displayfd",
                str(wr),
                "-screen",
                "0",
                "1280x800x24",
                "-nolisten",
                "tcp",
            ],
            pass_fds=(wr,),
            stderr=subprocess.DEVNULL,
        )
        children.append(server)
        os.close(wr)
        with os.fdopen(rd) as stream:
            disp = ":" + stream.readline().strip()
        env = {
            **os.environ,
            "DISPLAY": disp,
            "FLTK_BACKEND": "x11",
            "HOME": directory,
            "XDG_CONFIG_HOME": directory + "/config",
            "XDG_RUNTIME_DIR": directory,
        }
        for key in ["WAYLAND_DISPLAY", "WAYLAND_SOCKET"]:
            env.pop(key, None)
        d = display.Display(disp)
        screen = d.screen()
        owner = screen.root.create_window(
            850,
            400,
            200,
            160,
            0,
            screen.root_depth,
            X.InputOutput,
            X.CopyFromParent,
            event_mask=X.PropertyChangeMask | X.StructureNotifyMask,
        )
        owner.map()
        d.sync()
        atom = d.intern_atom
        offered = {}
        received = []

        def message(target, name, values):
            target.send_event(
                protocol.event.ClientMessage(
                    window=target,
                    client_type=atom(name),
                    data=(32, list(values) + [0] * (5 - len(values))),
                )
            )
            d.flush()

        def pump(timeout=0.04):
            readable, _, _ = select.select(
                [d.fileno()] + list(buffers), [], [], timeout
            )
            for fd in readable:
                if fd == d.fileno():
                    continue
                chunk = os.read(fd, 65536)
                if not chunk:
                    continue
                buffers[fd] += chunk
                while b"\n" in buffers[fd]:
                    line, buffers[fd] = buffers[fd].split(b"\n", 1)
                    inbox[fd].append(json.loads(line))
            while d.pending_events():
                e = d.next_event()
                events.append(e)
                if e.type == X.SelectionRequest:
                    requestor = e.requestor
                    prop = e.property or e.target
                    if e.target == atom("TARGETS"):
                        requestor.change_property(
                            prop, Xatom.ATOM, 32, [atom(k) for k in offered]
                        )
                    elif e.target in [atom(k) for k in offered]:
                        requestor.change_property(
                            prop, e.target, 8, offered[d.get_atom_name(e.target)]
                        )
                    else:
                        prop = X.NONE
                    requestor.send_event(
                        protocol.event.SelectionNotify(
                            time=e.time,
                            requestor=requestor,
                            selection=e.selection,
                            target=e.target,
                            property=prop,
                        )
                    )
                    d.flush()
                if (
                    e.type == X.ClientMessage
                    and e.client_type == atom("XdndPosition")
                    and e.window.id == owner.id
                ):
                    source = d.create_resource_object("window", e.data[1][0])
                    message(
                        source,
                        "XdndStatus",
                        [owner.id, 3, 0, 0, atom("XdndActionCopy")],
                    )
                if (
                    e.type == X.ClientMessage
                    and e.client_type == atom("XdndDrop")
                    and e.window.id == owner.id
                ):
                    owner.convert_selection(
                        atom("XdndSelection"),
                        atom("text/uri-list"),
                        atom("_FSVR_XDROP"),
                        X.CurrentTime,
                    )
                    d.flush()
                if e.type == X.SelectionNotify:
                    if e.property:
                        value = owner.get_full_property(e.property, X.AnyPropertyType)
                        received.append((e.target, value))

        def wait(fn, seconds=12):
            until = time.monotonic() + seconds
            while time.monotonic() < until:
                value = fn()
                if value:
                    return value
                pump()
            raise AssertionError("Timed out waiting for native file transfer")

        def fixture():
            p = subprocess.Popen(
                [str(root / "build-fltk/fltk/fsv-clipboard-fixture")],
                env=env,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            children.append(p)
            os.set_blocking(p.stdout.fileno(), False)
            buffers[p.stdout.fileno()] = b""
            inbox[p.stdout.fileno()] = []
            wait(lambda: inbox[p.stdout.fileno()])
            assert inbox[p.stdout.fileno()].pop(0)["event"] == "ready"
            return p

        def send(p, obj):
            p.stdin.write((json.dumps(obj) + "\n").encode())
            p.stdin.flush()

        def answer(p):
            result = wait(lambda: inbox[p.stdout.fileno()])
            result = result.pop(0)
            assert result.get("ok", True), result
            return result

        def ask(p, obj):
            send(p, obj)
            return answer(p)

        def window(p):
            for w in screen.root.query_tree().children:
                prop = w.get_full_property(atom("_NET_WM_PID"), X.AnyPropertyType)
                if prop is not None and prop.value[0] == p.pid and w.get_wm_name():
                    return w

        a, b = fixture(), fixture()
        wa = wait(lambda: window(a))
        wb = wait(lambda: window(b))
        paths = [directory + "/český # %\n日本語.txt", directory + "/space folder"]
        uris = "".join(
            "file://" + quote(path, safe="/") + "\r\n" for path in paths
        ).encode()
        offered = {"text/uri-list": uris, "application/x-kde-cutselection": b"1"}
        owner.set_selection_owner(atom("CLIPBOARD"), X.CurrentTime)
        d.sync()
        result = ask(a, {"op": "paste"})
        assert result["paths"] == paths and result["cut"], result
        ask(a, {"op": "copy", "paths": paths, "cut": False})
        result = ask(b, {"op": "paste"})
        assert result["paths"] == paths and not result["cut"]
        received.clear()
        owner.convert_selection(
            atom("CLIPBOARD"), atom("TARGETS"), atom("_FSVR_TARGETS"), X.CurrentTime
        )
        d.flush()
        value = wait(lambda: received)[-1][1]
        assert all(
            atom(name) in value.value
            for name in [
                "text/uri-list",
                "x-special/gnome-copied-files",
                "application/x-kde-cutselection",
            ]
        )
        large = [
            directory + "/" + str(n) + "_" + "x" * 180 + ".txt" for n in range(700)
        ]
        ask(a, {"op": "copy", "paths": large, "cut": True})
        result = ask(b, {"op": "paste"})
        assert result["paths"] == large and result["cut"], (
            "INCR must preserve a large file list"
        )
        # Independent XDND source -> native receiver, without using FLTK's text decoding.
        offered = {"text/uri-list": uris}
        owner.set_selection_owner(atom("XdndSelection"), X.CurrentTime)
        d.sync()
        message(wa, "XdndEnter", [owner.id, 5 << 24, atom("text/uri-list"), 0, 0])
        geo = wa.get_geometry()
        packed = ((geo.x + 100) << 16) | (geo.y + 100)
        message(
            wa,
            "XdndPosition",
            [owner.id, 0, packed, X.CurrentTime, atom("XdndActionCopy")],
        )
        status = wait(
            lambda: next(
                (
                    e
                    for e in reversed(events)
                    if e.type == X.ClientMessage and e.client_type == atom("XdndStatus")
                ),
                None,
            )
        )
        assert status.data[1][1] & 1
        message(wa, "XdndDrop", [owner.id, 0, X.CurrentTime])
        dropped = wait(
            lambda: next(
                (v for v in inbox[a.stdout.fileno()] if v.get("event") == "drop"), None
            )
        )
        inbox[a.stdout.fileno()].remove(dropped)
        assert dropped["paths"] == paths and not dropped["cut"], dropped
        # Native source -> independent XDND target; literal newlines remain percent-escaped.
        owner.change_property(atom("XdndAware"), Xatom.ATOM, 32, [5])
        wa.configure(stack_mode=X.Above)
        d.sync()
        wa.set_input_focus(X.RevertToParent, X.CurrentTime)
        xtest.fake_input(d, X.MotionNotify, x=geo.x + 120, y=geo.y + 100)
        xtest.fake_input(d, X.ButtonPress, 1)
        d.sync()
        time.sleep(0.15)
        send(a, {"op": "drag", "paths": paths})
        time.sleep(0.15)
        events.clear()
        received.clear()
        xtest.fake_input(d, X.MotionNotify, x=900, y=450)
        d.sync()
        wait(
            lambda: any(
                e.type == X.ClientMessage
                and e.client_type == atom("XdndPosition")
                and e.window.id == owner.id
                for e in events
            )
        )
        xtest.fake_input(d, X.ButtonRelease, 1)
        d.sync()
        wait(lambda: received)
        assert received[-1][1].value == uris, (
            "Native source must serve the real URI-list MIME type"
        )
        answer(a)
        for p in [a, b]:
            ask(p, {"op": "quit"})
        print(
            "PASS external KDE clipboard, native multi-MIME ownership, large INCR transfer, and incoming/outgoing XDND with Unicode/newline filenames"
        )
    finally:
        for p in reversed(children):
            if p.poll() is None:
                p.terminate()
                try:
                    p.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    p.kill()
                    p.wait()
