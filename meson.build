# SPDX-License-Identifier: MIT
project('fsv-xr', 'cpp', version: '0.1.0', default_options: ['cpp_std=c++17', 'warning_level=2'])
qt = import('qt6')
deps = dependency('qt6', version: '>=6.8', modules: ['Core', 'Gui', 'Widgets', 'Qml', 'Quick', 'QuickWidgets', 'QuickControls2', 'Quick3D', 'Concurrent'])
xr = dependency('qt6', version: '>=6.10', modules: ['Quick3DXr'], required: get_option('xr'))
resources = qt.preprocess(qresources: 'src/qt/resources.qrc')
moc = qt.preprocess(moc_headers: ['src/qt/filesystem.h', 'src/qt/branchgeometry.h', 'src/qt/gestures.h', 'src/qt/domed.h', 'src/qt/appearance.h'])
executable('fsv-xr', ['src/qt/main.cpp', 'src/qt/domed.cpp', 'src/qt/appearance.cpp', 'src/qt/filesystem.cpp', 'src/qt/filesystemindex.cpp', 'src/qt/trash.cpp', 'src/qt/treelayout.cpp', 'src/qt/branchgeometry.cpp', 'src/qt/thumbnail.cpp', 'src/qt/gestures.cpp', resources, moc],
  dependencies: [deps, xr], cpp_args: ['-DFSV_HAS_XR=' + (xr.found() ? '1' : '0')], install: true)
install_data('data/org.fsvxr.fsv-xr.desktop', install_dir: get_option('datadir') / 'applications')
if xr.found()
  install_data('data/org.fsvxr.fsv-xr-openxr.desktop', install_dir: get_option('datadir') / 'applications')
endif
