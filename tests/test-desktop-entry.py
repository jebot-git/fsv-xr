#!/usr/bin/env python3
"""Exercise the real CMake install rule without building application binaries."""
from pathlib import Path
import subprocess
import tempfile
import unittest

HELPER = Path(__file__).resolve().parents[1] / 'cmake/DesktopEntry.cmake'


class DesktopEntry(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / 'source'
        self.source.mkdir()
        self.sessions = self.root / 'sessions'
        self.stage = self.root / 'stage'
        self.build = self.root / 'build'
        for name in ('dome.desktop', 'xr.desktop'):
            (self.source / name).write_text('[Desktop Entry]\nType=Application\nName=' + name + '\nExec=true\n')

    def run_cmake(self, *args, env=None):
        import os
        result = subprocess.run(['cmake', *map(str, args)], env={**os.environ, **(env or {})},
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def configure(self, enabled='TRUE', isolated=True, datadir='share'):
        (self.source / 'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.21)\nproject(entries NONE)\n'
            f'set(CMAKE_INSTALL_DATADIR "{datadir}")\ninclude("{HELPER}")\n'
            f'de_xr_install_desktop_entry("{self.source}/dome.desktop" "{self.source}/xr.desktop" {enabled})\n')
        args = ['-S', self.source, '-B', self.build, '-DCMAKE_INSTALL_PREFIX=/usr/local']
        if isolated:
            args.append(f'-DDE_XR_SESSION_ROOT={self.sessions}')
        self.run_cmake(*args)

    def install(self, expected, prefix='/usr/local', applications=None):
        self.run_cmake('--install', self.build, '--prefix', prefix,
                       '--component', 'DesktopEntry', env={'DESTDIR': str(self.stage)})
        directory = self.stage / (applications or (prefix.lstrip('/') + '/share/applications'))
        self.assertEqual(sorted(p.name for p in directory.glob('*.desktop')), [expected])
        manifest = (self.build / 'install_manifest_DesktopEntry.txt').read_text()
        self.assertIn(expected, manifest)
        self.assertNotIn('xr.desktop' if expected == 'dome.desktop' else 'dome.desktop', manifest)

    def session(self, base, hidden=False, directory='usr/share/wayland-sessions', name='dome'):
        path = base / directory / (name + '.desktop')
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('[Desktop Entry]\nName=DomeD\n' + ('Hidden=true\n' if hidden else ''))
        return path

    def test_session_changes_without_reconfigure_and_removes_old_entry(self):
        self.configure()
        self.install('xr.desktop')
        session = self.session(self.sessions)
        self.install('dome.desktop')
        session.unlink()
        self.install('xr.desktop')

    def test_both_sessions_prefer_domed_and_hidden_domed_falls_back(self):
        self.session(self.sessions, directory='usr/share/xsessions', name='plasma')
        dome = self.session(self.sessions, directory='usr/local/share/wayland-sessions')
        self.configure()
        self.install('dome.desktop')
        dome.write_text('[Desktop Entry]\nHidden=true\n')
        self.install('xr.desktop')

    def test_session_delivered_in_same_stage(self):
        self.configure(isolated=False)
        self.session(self.stage)
        self.install('dome.desktop')

    def test_install_prefix_and_symlink_cleanup(self):
        self.configure()
        directory = self.stage / 'opt/apps/share/applications'
        directory.mkdir(parents=True)
        external = self.root / 'external'
        external.write_text('keep')
        (directory / 'dome.desktop').symlink_to(external)
        self.install('xr.desktop', prefix='/opt/apps')
        self.assertEqual(external.read_text(), 'keep')

    def test_disabled_xr_keeps_desktop_and_removes_stale_xr(self):
        self.configure(enabled='FALSE')
        directory = self.stage / 'usr/local/share/applications'
        directory.mkdir(parents=True)
        (directory / 'xr.desktop').write_text('old')
        self.install('dome.desktop')

    def test_absolute_datadir(self):
        self.configure(datadir='/opt/data')
        self.install('xr.desktop', applications='opt/data/applications')


class MesonDesktopEntry(unittest.TestCase):
    def test_real_meson_install_and_session_changes(self):
        import os
        import shutil
        if not shutil.which('meson'):
            self.skipTest('meson unavailable')
        project = HELPER.parent.parent
        script = project / 'scripts/install-desktop-entry.py'
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source, build, stage, sessions = (root / name for name in ('source', 'build', 'stage', 'sessions'))
            source.mkdir()
            (source / 'meson.build').write_text(
                "project('desktop-entries')\n"
                f"meson.add_install_script('{script}', "
                f"'{project}/data/org.fsvxr.fsvr.desktop', "
                f"'{project}/data/org.fsvxr.fsvr-openxr.desktop', 'true', 'share')\n")
            subprocess.run(['meson', 'setup', str(build), str(source), '--prefix=/usr/local'],
                           check=True, capture_output=True)
            env = {**os.environ, 'DESTDIR': str(stage), 'DE_XR_SESSION_ROOT': str(sessions)}
            applications = stage / 'usr/local/share/applications'
            for domed in (False, True, False):
                session = sessions / 'usr/share/wayland-sessions/dome.desktop'
                session.parent.mkdir(parents=True, exist_ok=True)
                if domed:
                    session.write_text('[Desktop Entry]\nName=DomeD\n')
                else:
                    session.unlink(missing_ok=True)
                subprocess.run(['meson', 'install', '-C', str(build), '--no-rebuild'],
                               check=True, capture_output=True, env=env)
                expected = 'org.fsvxr.fsvr.desktop' if domed else 'org.fsvxr.fsvr-openxr.desktop'
                self.assertEqual([path.name for path in applications.glob('*.desktop')], [expected])


if __name__ == '__main__':
    unittest.main()
