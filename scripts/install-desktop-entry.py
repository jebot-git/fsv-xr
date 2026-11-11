#!/usr/bin/env python3
"""Meson install-time counterpart of cmake/DesktopEntry.cmake."""
import os
from pathlib import Path
import re
import shutil
import sys


def install(dome, xr, xr_enabled, datadir):
    prefix = Path(os.environ['MESON_INSTALL_PREFIX'])
    destdir = os.environ.get('DESTDIR', '')
    session_root = os.environ.get('DE_XR_SESSION_ROOT', '')
    roots = [session_root] if session_root else [destdir, '/']
    domed = False
    for root in roots:
        for data in (prefix / 'share', Path('/usr/local/share'), Path('/usr/share')):
            for kind in ('wayland-sessions', 'xsessions'):
                for session in ('dome', 'domed'):
                    path = Path(root) / str(data).lstrip('/') / kind / (session + '.desktop')
                    if path.is_file() and not re.search(r'^\s*Hidden\s*=\s*true\s*$', path.read_text(), re.M):
                        domed = True
    selected, obsolete = (dome, xr) if domed or not xr_enabled else (xr, dome)
    destination = prefix / datadir / 'applications'
    destination = Path(destdir) / str(destination).lstrip('/')
    destination.mkdir(parents=True, exist_ok=True)
    target = destination / selected.name
    # Avoid following an old launcher symlink into an unrelated file.
    target.unlink(missing_ok=True)
    shutil.copyfile(selected, target)
    target.chmod(0o644)
    (destination / obsolete.name).unlink(missing_ok=True)


if __name__ == '__main__':
    install(Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3] == 'true', sys.argv[4])
