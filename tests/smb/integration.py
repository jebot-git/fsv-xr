#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Real loopback Samba/GVfs test. All server state, bus and mounts are private.

Needs rootless Podman, /dev/fuse, and GVfs SMB/FUSE. Build the adjacent
Containerfile as localhost/fsvr-smb-test:latest first. --deps accepts an extracted Fedora runtime for unprivileged
pre-install testing. No host shares, network credentials or services are used.
"""
import argparse
import json
import re
import os
from pathlib import Path
import select
import signal
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
if os.environ.get('FSVR_PRIVATE_SMB_TEST') != '1':
    os.execvpe('dbus-run-session', ['dbus-run-session', '--', sys.executable, *sys.argv],
               {**os.environ, 'FSVR_PRIVATE_SMB_TEST': '1'})
signal.pthread_sigmask(signal.SIG_SETMASK, [])
parser = argparse.ArgumentParser()
parser.add_argument('--deps', type=Path)
parser.add_argument('--ui', type=Path)
parser.add_argument('--wayland', action='store_true')
parser.add_argument('--container-image', default='localhost/fsvr-smb-test:latest')
args = parser.parse_args()
processes = []
PASSWORD = 'local-fixture-password'


def start(command, env, log):
    handle = log.open('w')
    p = subprocess.Popen(command, env=env, stdout=handle, stderr=subprocess.STDOUT, start_new_session=True)
    handle.close()
    processes.append(p)
    p.private_group = True
    return p


with tempfile.TemporaryDirectory(prefix='fsvr-smb-') as directory:
    base = Path(directory)
    base.chmod(0o755)
    env = {**os.environ, 'HOME': directory, 'XDG_CONFIG_HOME': str(base/'config'),
           'XDG_CACHE_HOME': str(base/'cache'), 'XDG_DATA_HOME': str(base/'data'),
           'XDG_RUNTIME_DIR': str(base/'runtime'), 'XDG_STATE_HOME': str(base/'state'),
           'GVFS_REMOTE_VOLUME_MONITOR_IGNORE': '1',
           'DBUS_SYSTEM_BUS_ADDRESS': 'unix:path=/nonexistent/fsvr-smb-system-bus',
           'XR_RUNTIME_JSON': '/nonexistent/fsvr-smb-runtime.json'}
    for name in ['private', 'lock', 'state', 'cache', 'pid', 'share', 'ncalrpc', 'runtime', 'config', 'data']:
        (base/name).mkdir(mode=0o700)
    for name in ['lock', 'state', 'cache', 'pid', 'ncalrpc']:
        (base/name).chmod(0o755)
    (base/'share/český 日本語.txt').write_text('SMB fixture')
    (base/'share/český 日本語.txt').chmod(0o644)
    (base/'share').chmod(0o777)
    (base/'share/folder').mkdir()
    (base/'share/folder').chmod(0o755)
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0)); port = sock.getsockname()[1]
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0)); rpc_port = min(sock.getsockname()[1], 65520)
    config = '[global]\nserver role = standalone server\nsecurity = user\n'
    config += f'smb ports = {port}\nserver min protocol = SMB2\nmap to guest = Bad User\nguest account = nobody\nrestrict anonymous = 0\n'
    config += f'rpc server dynamic port range = {rpc_port}-{rpc_port+10}\n'
    # RPC workers start independently and use Samba's compiled default paths.
    # Keep those defaults inside the disposable container, not the host mount.
    config += 'log file = /var/log/samba/log.%m\nload printers = no\ndisable spoolss = yes\n'
    for name, guest, readonly in [('public', 'yes', 'no'), ('secure', 'no', 'no'), ('readonly', 'yes', 'yes')]:
        config += f'[{name}]\npath = {base}/share\nguest ok = {guest}\nread only = {readonly}\n'
    (base/'smb.conf').write_text(config)
    (base/'smb.conf').chmod(0o644)
    (base/'password-input').write_text(PASSWORD+'\n'+PASSWORD+'\n')
    container_name = 'fsvr-smb-test-'+str(os.getpid())
    try:
        # A complete Samba installation keeps the real named-pipe RPC service
        # available for share enumeration. Only one loopback port is published.
        server = '''import subprocess, sys, shutil
from pathlib import Path
source, base = sys.argv[1:]
config = '/etc/samba/smb.conf'
shutil.copyfile(source, config)
Path('/run/samba').mkdir(parents=True, exist_ok=True)
Path('/run/samba/ncalrpc').mkdir(parents=True, exist_ok=True)
Path('/var/lib/samba/private').mkdir(parents=True, exist_ok=True)
Path('/var/lib/samba/lock').mkdir(parents=True, exist_ok=True)
Path('/var/cache/samba').mkdir(parents=True, exist_ok=True)
secret = Path(base+'/password-input')
subprocess.run(['smbpasswd', '-c', config, '-L', '-a', '-s', 'root'], input=secret.read_text(), text=True, check=True, capture_output=True)
secret.unlink()
subprocess.run(['smbd', '--foreground', '--no-process-group', '--debug-stdout', '--configfile='+config], check=True)
'''
        start(['podman', '--cgroup-manager=cgroupfs', '--events-backend=file', 'run', '--rm', '--name', container_name, '--security-opt=label=disable',
               '-p', f'127.0.0.1:{port}:{port}', '-v', f'{base}:{base}:rw', args.container_image,
               '/usr/bin/python3', '-c', server, str(base/'smb.conf'), directory], dict(os.environ), base/'smbd.log')
        for _ in range(100):
            try:
                with socket.create_connection(('127.0.0.1', port), timeout=.1): break
            except OSError: time.sleep(.1)
        else: raise AssertionError('Private Samba server did not start')
        prefix = args.deps.resolve() if args.deps else Path('/')
        daemon = prefix/'usr/libexec/gvfsd'
        fuse = prefix/'usr/libexec/gvfsd-fuse'
        if args.deps:
            env['LD_LIBRARY_PATH'] = str(prefix/'usr/lib64/gvfs') + ':' + env.get('LD_LIBRARY_PATH', '')
            env['GIO_EXTRA_MODULES'] = str(prefix/'usr/lib64/gio/modules')
            mounts = base/'mountables'; mounts.mkdir()
            for p in (prefix/'usr/share/gvfs/mounts').glob('*.mount'):
                (mounts/p.name).write_text(p.read_text().replace('Exec=/usr/libexec/', 'Exec='+str(prefix/'usr/libexec')+'/'))
            env['GVFS_MOUNTABLE_DIR'] = str(mounts)
            env['GSETTINGS_SCHEMA_DIR'] = str(prefix/'usr/share/glib-2.0/schemas')
            subprocess.run(['glib-compile-schemas', env['GSETTINGS_SCHEMA_DIR']], check=True, capture_output=True)
        start([str(daemon), '--no-fuse'], env, base/'gvfs.log')
        for _ in range(40):
            reply = subprocess.run(['gdbus', 'call', '--session', '--dest', 'org.freedesktop.DBus',
                '--object-path', '/org/freedesktop/DBus', '--method', 'org.freedesktop.DBus.NameHasOwner',
                'org.gtk.vfs.Daemon'], env=env, capture_output=True, text=True)
            if 'true' in reply.stdout: break
            time.sleep(.05)
        else: raise AssertionError('Private GVfs daemon did not start')
        (base/'runtime/gvfs').mkdir()
        start([str(fuse), str(base/'runtime/gvfs'), '-f'], env, base/'fuse.log')
        time.sleep(1)
        uri = f'smb://127.0.0.1:{port}'

        def request(address, command='connect', mode='correct'):
            p = subprocess.Popen([sys.executable, str(ROOT/'services/fsvr-network.py')], env=env,
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            processes.append(p)
            def send(value):
                p.stdin.write(json.dumps(value)+'\n'); p.stdin.flush()
            send(dict(command=command, uri=address))
            until = time.monotonic()+20
            attempts = 0
            while time.monotonic() < until:
                if not select.select([p.stdout], [], [], .2)[0]: continue
                line = p.stdout.readline()
                if not line: raise AssertionError('Helper exited: '+p.stderr.read())
                event = json.loads(line)
                if event['event'] == 'password':
                    attempts += 1
                    if mode == 'cancel': send(dict(command='cancel'))
                    else: send(dict(command='authenticate', username='root', domain='WORKGROUP',
                                    password='wrong-password' if mode == 'retry' and attempts == 1 else PASSWORD,
                                    anonymous=mode == 'guest'))
                elif event['event'] == 'result':
                    p.wait(timeout=5)
                    return event, attempts
            raise AssertionError('Helper timed out')

        cancelled, _ = request(uri+'/secure', mode='cancel')
        assert 'error' in cancelled, cancelled
        secure, attempts = request(uri+'/secure', mode='retry')
        assert secure.get('kind') == 'connected' and attempts >= 2, (secure, attempts)
        remote = Path(secure['path'])
        assert (remote/'český 日本語.txt').read_text() == 'SMB fixture'
        (remote/'upload.txt').write_text('uploaded')
        assert (base/'share/upload.txt').read_text() == 'uploaded'
        (remote/'upload.txt').rename(remote/'renamed.txt')
        assert (base/'share/renamed.txt').is_file()
        (remote/'new-folder').mkdir(); assert (base/'share/new-folder').is_dir()
        (remote/'renamed.txt').unlink()
        print('PASS cancelled auth, wrong-password retry, Unicode read, upload, rename, mkdir and delete', flush=True)
        readonly, _ = request(uri+'/readonly', mode='guest')
        assert readonly.get('kind') == 'connected', readonly
        try: (Path(readonly['path'])/'blocked.txt').write_text('blocked')
        except OSError: pass
        else: raise AssertionError('Read-only share accepted a write')
        print('PASS read-only permissions', flush=True)
        if args.ui:
            env['FSVR_SMB_TEST_URI'] = uri+'/secure'
            env['FSVR_SMB_TEST_PASSWORD'] = PASSWORD
            env['FSVR_SMB_TEST_SOURCE'] = str(base/'share')
            before_ui, _ = request(uri+'/secure', 'disconnect')
            assert before_ui.get('kind') == 'disconnected', before_ui
            ui_env = {**env, 'FLTK_BACKEND': 'x11', 'LIBGL_ALWAYS_SOFTWARE': '1', 'XDG_CURRENT_DESKTOP': 'Dome'}
            command = ['xvfb-run', '-a', str(args.ui.resolve())]
            if args.wayland:
                rd, wr = os.pipe()
                display = subprocess.Popen(['Xvfb', '-displayfd', str(wr), '-screen', '0', '1280x900x24', '-nolisten', 'tcp'],
                                           pass_fds=(wr,), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                processes.append(display); os.close(wr)
                with os.fdopen(rd) as stream: ui_env['DISPLAY'] = ':'+stream.readline().strip()
                ui_env.pop('WAYLAND_DISPLAY', None); ui_env.pop('WAYLAND_SOCKET', None)
                ui_env.update(DOME_START_XR='0', DOME_LOGIN_SESSION='0', DOME_APPEARANCE_SERVICE='0', DOME_RENDERER='gles')
                with socket.socket() as sock:
                    sock.bind(('127.0.0.1', 0)); ui_env['DOME_PREVIEW_PORT'] = str(sock.getsockname()[1])
                compositor = start(['/usr/local/bin/dome-compositor', '--backend', 'nested'], ui_env, base/'compositor.log')
                for _ in range(150):
                    match = re.search(r'Dome Wayland socket: (\S+)', (base/'compositor.log').read_text())
                    if match: break
                    assert compositor.poll() is None, 'Private compositor exited'
                    time.sleep(.1)
                else: raise AssertionError('Private Wayland compositor did not start')
                ui_env.update(WAYLAND_DISPLAY=match[1], FLTK_BACKEND='wayland')
                ui_env['FSVR_SMB_TEST_HOST_DISPLAY'] = ui_env['DISPLAY']
                ui_env.pop('DISPLAY', None)
                command = [str(args.ui.resolve())]
            subprocess.run(command, env=ui_env, check=True, timeout=45)
        shares, _ = request(uri+'/')
        assert shares.get('kind') == 'shares' and any(e['uri'] == uri+'/secure' for e in shares['entries']), shares
        print('PASS server share enumeration', flush=True)
        disconnected, _ = request(uri+'/secure', 'disconnect')
        assert disconnected.get('kind') == 'disconnected', disconnected
        print('PASS normal disconnect; private fixture complete', flush=True)
    except BaseException:
        logs = subprocess.run(['podman', '--cgroup-manager=cgroupfs', '--events-backend=file', 'exec', container_name,
                               '/bin/sh', '-c', 'tail -n 25 /var/log/samba/log.* 2>/dev/null'], capture_output=True, text=True)
        print('Container Samba logs:', logs.stdout[-9000:], logs.stderr[-1000:])
        for name in ['gvfs.log', 'fuse.log', 'smbd.log', 'rpc.log', 'log.rpcd_classic']:
            print(name, (base/name).read_text(errors='replace')[-9000:] if (base/name).exists() else 'missing')
        raise
    finally:
        for p in reversed(processes):
            if getattr(p, 'private_group', False):
                try: os.killpg(p.pid, signal.SIGTERM)
                except ProcessLookupError: pass
            if p.poll() is None:
                try: p.terminate(); p.wait(timeout=3)
                except subprocess.TimeoutExpired: p.kill(); p.wait()
        subprocess.run(['fusermount3', '-u', str(base/'runtime/gvfs')], capture_output=True)
        subprocess.run(['podman', '--cgroup-manager=cgroupfs', '--events-backend=file', 'rm', '-f', container_name], capture_output=True)
