#!/usr/bin/env python3
"""Run native FLTK model UI on an isolated nested DomeD Wayland compositor."""
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time

signal.pthread_sigmask(signal.SIG_SETMASK, [])
if os.environ.get('FSVR_WL_TEST_BUS') != '1':
    os.execvpe('dbus-run-session', ['dbus-run-session','--',sys.executable,*sys.argv],
              {**os.environ,'FSVR_WL_TEST_BUS':'1'})
root = Path(__file__).resolve().parents[2]
out = root/'build-fltk/gltf-wayland-test'
out.mkdir(parents=True, exist_ok=True)
children = []
try:
    with tempfile.TemporaryDirectory(prefix='fsvr-wayland-') as tmp:
        os.chmod(tmp, 0o700)
        rd, wr = os.pipe()
        xvfb = subprocess.Popen(['Xvfb','-displayfd',str(wr),'-screen','0','1600x1000x24','-nolisten','tcp'],
                                pass_fds=(wr,),stderr=subprocess.DEVNULL)
        children.append(xvfb)
        os.close(wr)
        with os.fdopen(rd) as stream:
            display = ':' + stream.readline().strip()
        with socket.socket() as sock:
            sock.bind(('127.0.0.1',0))
            port = sock.getsockname()[1]
        env = {**os.environ,'DISPLAY':display,'XDG_RUNTIME_DIR':tmp,'XDG_CURRENT_DESKTOP':'Dome',
               'XDG_CONFIG_HOME':tmp+'/config','XDG_CACHE_HOME':tmp+'/cache','XDG_STATE_HOME':tmp+'/state',
               'LIBGL_ALWAYS_SOFTWARE':os.environ.get('LIBGL_ALWAYS_SOFTWARE','1'),'DOME_START_XR':'0','DOME_LOGIN_SESSION':'0',
               'DOME_APPEARANCE_SERVICE':'0','DOME_RENDERER':'gles','DOME_PREVIEW_PORT':str(port),
               'DBUS_SYSTEM_BUS_ADDRESS':'unix:path=/nonexistent/fsvr-private-system-bus',
               'XR_RUNTIME_JSON':'/nonexistent/fsvr-private-runtime.json'}
        for key in ('WAYLAND_DISPLAY','WAYLAND_SOCKET','FLTK_BACKEND'):
            env.pop(key,None)
        with (out/'compositor.log').open('w') as log:
            compositor = subprocess.Popen(['/usr/local/bin/dome-compositor','--backend','nested'],
                                          env=env,stdout=log,stderr=log)
            children.append(compositor)
            deadline = time.monotonic()+15
            while time.monotonic()<deadline:
                match = re.search(r'Dome Wayland socket: (\S+)',(out/'compositor.log').read_text())
                if match:
                    break
                assert compositor.poll() is None, 'Private compositor exited'
                time.sleep(.1)
            else:
                raise AssertionError('Private Wayland compositor did not start')
            env.update(WAYLAND_DISPLAY=match[1], FLTK_BACKEND='wayland', WAYLAND_DEBUG='client')
            with (out/'preview.log').open('w') as capture:
                app = subprocess.Popen([os.environ.get('FSVR_WAYLAND_TEST_BINARY',str(root/'build-fltk/fltk/fsv-model-ui-test')),str(out/'preview.ppm')],
                                       env=env,stdout=capture,stderr=capture)
                children.append(app)
                app.wait(timeout=120 if 'FSVR_WAYLAND_TEST_BINARY' in os.environ else 35)
            trace=(out/'preview.log').read_text()
            assert app.returncode == 0, trace[-4000:]
            assert 'set_address' in trace, 'Universal appmenu was not exported over Wayland'
            assert 'xdg_surface' in trace, 'No native Wayland window'
            print('PASS native Wayland directory navigation stress and universal menu' if 'FSVR_WAYLAND_TEST_BINARY' in os.environ else 'PASS native Wayland live preview, GPU capture, reload, navigation and universal menu')
finally:
    for child in reversed(children):
        if child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=4)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
