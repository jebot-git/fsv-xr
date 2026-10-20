# SPDX-License-Identifier: MIT
project('fsvr', 'cpp', version: '0.1.0', default_options: ['cpp_std=c++17', 'warning_level=2', 'buildtype=release', 'b_lto=true', 'b_ndebug=if-release'])
qt = import('qt6')
deps = dependency('qt6', version: '>=6.8', modules: ['Core', 'Gui', 'Widgets', 'Qml', 'Quick', 'QuickWidgets', 'QuickControls2', 'Quick3D', 'Concurrent'])
xr = dependency('qt6', version: '>=6.10', modules: ['Quick3DXr'], required: get_option('xr'))
archive_dep = dependency('libarchive')
resources = qt.preprocess(qresources: 'src/qt/resources.qrc')
moc = qt.preprocess(moc_headers: ['src/qt/filesystem.h', 'src/qt/branchgeometry.h', 'src/qt/gestures.h', 'src/qt/domed.h', 'src/qt/domemenu.h', 'src/qt/appearance.h'])
executable('fsvr', ['src/qt/archivebrowser.cpp', 'src/qt/archives.cpp', 'src/qt/main.cpp', 'src/qt/domed.cpp', 'src/qt/domemenu.cpp', 'src/qt/appearance.cpp', 'src/qt/filesystem.cpp', 'src/qt/launchcontext.cpp', 'src/qt/filesystemindex.cpp', 'src/qt/trash.cpp', 'src/qt/treelayout.cpp', 'src/qt/branchgeometry.cpp', 'src/qt/thumbnail.cpp', 'src/qt/gestures.cpp', resources, moc],
  dependencies: [deps, xr, archive_dep], cpp_args: ['-DFSV_HAS_XR=' + (xr.found() ? '1' : '0')], install: true)
install_data('data/org.fsvxr.fsvr.desktop', install_dir: get_option('datadir') / 'applications')
if xr.found()
  install_data('data/org.fsvxr.fsvr-openxr.desktop', install_dir: get_option('datadir') / 'applications')
endif

install_data('data/dome-mimeapps.list', install_dir: get_option('datadir') / 'applications')

install_data('scripts/set-dome-default.py', install_dir: get_option('bindir'), rename: 'fsvr-dome-default', install_mode: 'rwxr-xr-x')

install_man('docs/man/fsvr.1')
