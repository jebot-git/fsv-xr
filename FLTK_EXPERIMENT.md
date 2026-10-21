# FLTK 1.4.5 experiment

OpenGL FLTK filesystem browser with native 3D MapV/TreeV/DiscV scenes, ray picking, orbit/pan/zoom, tabs/history, async file operations and archive browsing/extraction/compression.

This experimental default is preserved in the private `fltk-vr` source
collection. The collection provides a staged local installer with backups;
running desktop and display-manager sessions are never restarted by installation.

```sh
cmake -S . -B build-fltk -DCMAKE_PREFIX_PATH=<FLTK prefix>
cmake --build build-fltk --parallel 4
```

The Qt frontend remains selectable with CMake `-DDE_XR_TOOLKIT=Qt6`.
Use the isolated FLTK **1.4.5** runtime in the parent `_toolkit`; prepend its `lib`
to `LD_LIBRARY_PATH` so the login session's older runtime cannot override it.
The parent `run` launcher handles this. Local bitmap family/style/strike, popup
dismissal, deferred window close, EGL depth and borderless app-ID patches are required.

DomeD supplies the working XR Wayland seat and GPU transport. Standalone hand/controller OpenXR remains unported and --xr reports this explicitly. Trash restore/empty, places, drag/drop and complete file-operation UI parity remain incomplete.

View → Sort by and the folder context menu support natural Name, Size, Modified,
Type, Created, Accessed and Extension ordering, ascending/descending, folders first
and case-sensitive names. Unknown dates remain last. Preferences are saved to
`$XDG_CONFIG_HOME/fsvr/fltk.json`. Sorting applies to the list and sibling groups
in all three layouts, retaining selection and parent/child integrity. Type labels
use GIO content metadata; creation dates use statx when available, otherwise
show Unavailable. File browsing does not require GTK.

Application menu commands are exported over DBusMenu/KDE AppMenu where applicable.
DomeD appearance updates apply live, including native FreeType bitmap strikes.
OpenGL widgets cache glyph textures after rasterization. Bitmap shaping/bidi and
per-glyph fallback, accessibility/IME and broad fractional-scale coverage remain
incomplete. Tests cover selected desktop and DomeD XR input paths, not every
interaction. The local WiVRn server has no connected client: physical Intel GPU
OpenXR validation uses an isolated Monado simulated HMD and does not validate
WiVRn network streaming.

The parent `README.md` contains the suite report, validation commands and remaining
limitations; evidence is in the parent `artifacts/` directory.
