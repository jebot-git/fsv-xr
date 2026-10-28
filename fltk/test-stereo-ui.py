#!/usr/bin/env python3
"""Isolated real DomeD/Wayland stereo rendering; no active session changes."""
import json, os, re, signal, socket, subprocess, sys, tempfile, threading, time, urllib.request
from pathlib import Path
signal.pthread_sigmask(signal.SIG_SETMASK, [])
if os.environ.get('FSVR_STEREO_BUS') != '1':
    os.execvpe('dbus-run-session', ['dbus-run-session', '--', sys.executable, *sys.argv], {**os.environ, 'FSVR_STEREO_BUS':'1'})
exe, compositor = [Path(p).resolve() for p in sys.argv[1:3]]
out = exe.parent/'stereo-qa'; out.mkdir(exist_ok=True)
children=[]; logs=[]; stop=threading.Event()
def wait(f):
    deadline=time.monotonic()+20
    while time.monotonic()<deadline:
        try:
            value=f()
            if value:return value
        except (OSError,ValueError,KeyError):pass
        time.sleep(.05)
    raise AssertionError('Timed out; see '+str(out))
def start(args,env,name,**kwargs):
    log=(out/(name+'.log')).open('w'); logs.append(log)
    p=subprocess.Popen(list(map(str,args)),env=env,stdout=log,stderr=log,**kwargs);children.append(p);return p
try:
    with tempfile.TemporaryDirectory(prefix='fsvr-stereo-') as directory:
        root=Path(directory); root.chmod(0o700)
        rd,wr=os.pipe()
        start(['Xvfb','-displayfd',str(wr),'-screen','0','1600x1000x24','-nolisten','tcp'],os.environ,'xvfb',pass_fds=(wr,));os.close(wr)
        with os.fdopen(rd) as stream:display=':'+stream.readline().strip()
        with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
        env={**os.environ,'DISPLAY':display,'XDG_RUNTIME_DIR':directory,'XDG_CURRENT_DESKTOP':'Dome','XDG_CONFIG_HOME':directory+'/config','XDG_CACHE_HOME':directory+'/cache','XDG_STATE_HOME':directory+'/state','XDG_DATA_HOME':directory+'/data','DOME_START_XR':'0','DOME_START_IME':'0','DOME_LOGIN_SESSION':'0','DOME_SESSION_RESTORE':'0','DOME_APPEARANCE_SERVICE':'0','DOME_START_XWAYLAND':'0','DOME_RENDERER':'gles','LIBGL_ALWAYS_SOFTWARE':'1','DOME_PREVIEW_PORT':str(port),'DBUS_SYSTEM_BUS_ADDRESS':'unix:path=/nonexistent/fsvr-system-bus'}
        for key in ['WAYLAND_DISPLAY','WAYLAND_SOCKET','FLTK_BACKEND']:env.pop(key,None)
        start([compositor,'--backend','nested'],env,'compositor')
        desktop=wait(lambda:re.search(r'Dome Wayland socket: (\S+)',(out/'compositor.log').read_text()))[1]
        http=urllib.request.build_opener(urllib.request.ProxyHandler({}));base=f'http://127.0.0.1:{port}'
        def get(path):return json.load(http.open(base+path,timeout=2))
        def post(path,data):return json.load(http.open(urllib.request.Request(base+path,data=json.dumps(data).encode(),headers={'Content-Type':'application/json'}),timeout=2))
        def pulse():
            while not stop.is_set():
                try:post('/api/xr-session',{'id':78,'visible':True,'focused':True,'horizontal_fov_deg':90,'head':{'position_m':[0,1.6,0],'orientation_xyzw':[0,0,0,1]},'eyes':[{'position_m':[x,1.6,0],'orientation_xyzw':[0,0,0,1]} for x in [-.032,.032]]})
                except OSError:return
                stop.wait(.1)
        threading.Thread(target=pulse,daemon=True).start();wait(lambda:get('/api/xr-control')['visible'])
        post('/api/action',{'op':'xr_presentation','desktop':True})
        wait(lambda:get('/api/xr-control')['input_realm']=='xr')
        fixture=root/'fixture';fixture.mkdir()
        for n in range(8):
            folder=fixture/f'Folder {n}';folder.mkdir();(folder/'file.txt').write_text('Example contents'*100)
        marker=out/'toggle-ready';marker.unlink(missing_ok=True)
        for kind in ('input','chooser','custom'):
            for suffix in ('ready','checked'):(out/(kind+'-'+suffix)).unlink(missing_ok=True)
        app=start([exe,fixture,out],{**env,'WAYLAND_DISPLAY':desktop,'FLTK_BACKEND':'wayland'},'fsvr')
        for kind in ('input','chooser','custom'):
            wait(lambda:(out/(kind+'-ready')).exists() or app.poll() is not None)
            assert (out/(kind+'-ready')).exists(),(out/'fsvr.log').read_text()
            def dialog_state():
                windows=[w for w in get('/api/state')['scene']['windows'] if w['client_pid']==app.pid]
                main=next((w for w in windows if w['app_id']=='org.fsvxr.fsvr.Sbs'),None)
                dialog=next((w for w in windows if w['app_id']=='org.fsvxr.fsvr'),None)
                return (main,dialog) if main and dialog else None
            main,dialog=wait(dialog_state)
            assert main['stereo']=='side_by_side',main
            assert dialog['stereo']=='mono',dialog
            (out/(kind+'-checked')).touch()
        wait(lambda:marker.exists() or app.poll() is not None)
        assert marker.exists(),(out/'fsvr.log').read_text()
        window=next(w for w in get('/api/state')['scene']['windows'] if w['app_id']=='org.fsvxr.fsvr.Sbs')
        assert window['stereo']=='side_by_side',window
        post('/api/action',{'op':'xr_presentation','desktop':False})
        app.wait(timeout=20)
        assert app.returncode==0,(out/'fsvr.log').read_text()
        print((out/'fsvr.log').read_text())
finally:
    stop.set()
    for child in reversed(children):
        if child.poll() is None:child.terminate()
    for child in children:
        try:child.wait(timeout=5)
        except subprocess.TimeoutExpired:child.kill();child.wait()
    for log in logs:log.close()
