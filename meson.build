# SPDX-License-Identifier: MIT
project('fsvr', 'cpp', 'c', version: '0.1.0', default_options: ['cpp_std=c++17', 'warning_level=2', 'buildtype=release', 'b_lto=true', 'b_ndebug=if-release'])
qt = import('qt6')
deps = dependency('qt6', version: '>=6.8', modules: ['Core', 'Gui', 'Widgets', 'Qml', 'Quick', 'QuickWidgets', 'QuickControls2', 'Quick3D', 'Concurrent'])
xr = dependency('qt6', version: '>=6.10', modules: ['Quick3DXr'], required: get_option('xr'))
archive_dep = dependency('libarchive')
thumbnail_deps = [dependency('libpng'), dependency('libjpeg'), meson.get_compiler('c').find_library('gif')]
thumbnail_decode = static_library('fsv-thumbnail-decode', 'src/common/thumbnail-decode.c', dependencies: thumbnail_deps)

resources = qt.preprocess(qresources: 'src/qt/resources.qrc')
moc = qt.preprocess(moc_headers: ['src/qt/filesystem.h', 'src/qt/branchgeometry.h', 'src/qt/gestures.h', 'src/qt/domed.h', 'src/qt/domemenu.h', 'src/qt/appearance.h'])
executable('fsvr', ['src/qt/archivebrowser.cpp', 'src/qt/archives.cpp', 'src/qt/main.cpp', 'src/qt/domed.cpp', 'src/qt/domemenu.cpp', 'src/qt/appearance.cpp', 'src/qt/filesystem.cpp', 'src/qt/launchcontext.cpp', 'src/qt/filesystemindex.cpp', 'src/qt/trash.cpp', 'src/qt/treelayout.cpp', 'src/qt/branchgeometry.cpp', 'src/qt/thumbnail.cpp', 'src/qt/gestures.cpp', resources, moc],
  include_directories: include_directories('src/common'), link_with: thumbnail_decode, dependencies: [deps, xr, archive_dep] + thumbnail_deps, cpp_args: ['-DFSV_HAS_XR=' + (xr.found() ? '1' : '0')], install: true)
meson.add_install_script('scripts/install-desktop-entry.py',
  meson.current_source_dir() / 'data/org.fsvxr.fsvr.desktop',
  meson.current_source_dir() / 'data/org.fsvxr.fsvr-openxr.desktop',
  xr.found() ? 'true' : 'false', get_option('datadir'))

install_data('data/dome-mimeapps.list', install_dir: get_option('datadir') / 'applications')

install_data('scripts/set-dome-default.py', install_dir: get_option('bindir'), rename: 'fsvr-dome-default', install_mode: 'rwxr-xr-x')

install_man('docs/man/fsvr.1')
