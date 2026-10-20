# fsvr

Previously named fsv-xr.

A Qt6 filesystem visualizer rendered with Vulkan, forked from
[fsv](http://fsv.sourceforge.net/) by Daniel Richard G. The active implementation
is C++ and Qt Quick 3D in `src/qt`; the retired GTK3/OpenGL implementation is
preserved under `legacy/` and is excluded from both build systems.

## Build

Requires C++17, CMake 3.21+ (or Meson), Qt 6.8+ Core, GUI, Widgets, QML, Quick,
Quick Widgets, Quick Controls 2, Quick 3D, Concurrent, libarchive, and a Vulkan driver.
Standalone XR additionally requires Qt 6.10+ Quick 3D XR built with OpenXR/Vulkan,
an OpenXR loader, and a functioning runtime such as Monado or WiVRn. Qt Test is
needed when `BUILD_TESTING` is enabled (the CMake default).

On Fedora 44:

```sh
sudo dnf install gcc-c++ cmake ninja-build qt6-qtbase-devel \
  qt6-qtdeclarative-devel qt6-qtquick3d-devel vulkan-loader-devel openxr-devel libarchive-devel
cmake -S . -B build -G Ninja -DFSV_XR=ON
cmake --build build
ctest --test-dir build --output-on-failure
./build/fsvr /path/to/directory
sudo cmake --install build --prefix /usr/local
sudo update-desktop-database /usr/local/share/applications
```

Builds default to Release optimization (`-O3` with GCC/Clang). CMake enables
link-time optimization when the toolchain supports it; disable it with
`-DFSV_LTO=OFF`. Choose `-DCMAKE_BUILD_TYPE=Debug` or `RelWithDebInfo` explicitly
for debugging. Meson defaults to `--buildtype=release -Db_lto=true`; use
`-Db_lto=false` if the toolchain lacks LTO support. Existing Meson build
directories retain their settings. Update them with:

```sh
meson configure builddir --buildtype=release -Db_lto=true -Db_ndebug=if-release
```

The system install provides **fsvr** in the application menu, plus **fsvr
(OpenXR)** when standalone XR is built. Keep `/usr/local/bin` in the desktop
session's `PATH`. A per-user alternative is `cmake --install build --prefix "$HOME/.local"`. The main entry follows DomeD's selected desktop/XR Wayland realm and theme.
Desktop launches retain desktop presentation; XR launches use transparent scenes
and native context menus, including when a headset is already active. Its menu actions can explicitly select desktop or the
current DomeD session. The OpenXR entry starts its own headset session using the
active runtime; use the main entry inside DomeD. The install includes Dome-specific MIME defaults; run `fsvr-dome-default`
to enable the user override for DomeD without changing KDE/GNOME defaults.
The OpenXR runtime is unchanged.

Use `-DFSV_XR=OFF` to build without standalone XR. Desktop and DomeD modes still
work. Meson is also supported:

```sh
meson setup builddir -Dxr=auto
meson compile -C builddir
meson install -C builddir
```

The renderer explicitly selects Vulkan through Qt's rendering hardware interface
(RHI). It does not fall back to OpenGL. Vulkan rendering tests need a live display:

```sh
cmake -S . -B build -DFSV_DISPLAY_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## Explore

The left Places panel follows Dolphin's local bookmarks in
`$XDG_DATA_HOME/user-places.xbel`, including their names, icons and hidden state.
Without that file it offers the standard user folders. Trash opens an internal
`trash:/` tab with 3D objects, thumbnails, original locations and deletion dates.
It combines the home Trash and both standard Trash stores on mounted volumes
using the [freedesktop Trash format](https://specifications.freedesktop.org/trash/latest/).
Restore (also double-click) returns a whole file or folder to its original location;
restore a folder before browsing its contents. Existing destinations are never
overwritten; missing parent folders must be recreated first. Items with damaged
metadata remain visible and can be erased, but cannot be restored automatically.
Permanent deletion and Empty Trash require confirmation in desktop and XR menus.
Empty Trash covers all items on currently available volumes, including items
beyond the 2,500-item display limit; disconnected volumes are untouched.
Trash is refreshed live and bypasses the persistent filesystem index. File System and mounted
volumes under `/run/media`, `/media` and `/mnt` appear under Devices. Bookmarks
and mounts update live; clicking a local place navigates there. Other virtual KIO
locations (search and remote URLs) require a local mount and are omitted.

Directory tabs are available on desktop and in XR. Use **+** or Ctrl+T to open
a tab (or double-click empty space in the tab bar), Ctrl+Tab / Ctrl+Shift+Tab
to switch, and **×**, MMB on a tab, or Ctrl+W
to close. The last tab stays open. MMB on a Place or **Open in New Tab** on a
folder opens that location separately. Each tab remembers its location, layout,
selection, zoom, pan, rotation and tilt; up to 32 tabs can be open. Switching tabs
restores the in-memory scene immediately and refreshes it in the background.

Visited directories have a persistent metadata index under
`$XDG_CACHE_HOME/fsvr/index-v1` (normally `~/.cache/fsvr/index-v1`). On a
subsequent visit or application launch, the index supplies the first scene without
waiting for a full recursive scan. Cold locations publish an initial batch while
scanning continues. Cache reads, scans and atomic index writes run on a worker;
up to 64 root snapshots, each at most 8 MiB and 2,500 entries, are retained.
The index is a preview cache: every visit is revalidated against the live
filesystem, modifying operations wait for that validation, and F5 rebuilds the
current snapshot. Missing, corrupt or unwritable caches fall back to live scans.
Deleting the index directory clears it; it is rebuilt on subsequent visits.
There is no always-running whole-disk indexer. Thumbnail loading also reuses valid
freedesktop thumbnail caches and Qt's in-process image cache.

Map, Tree, and Disc layouts preserve parent/child nesting. Map uses a squarified
treemap with square-root size weights and a maximum sibling area contrast of
4:1, preventing tiny files beside large ones from becoming thin strips. Height
represents file size logarithmically. In Map and Disc, folder platforms contain
their children.
Tree follows the original fsv's platform-and-branch structure: each directory
gets a separate gold platform carrying its immediate file columns, and branches
connect it to child directory platforms farther back. Sibling subtrees occupy
separate lanes instead of being nested into a grid. The current directory also
has a labelled root platform. Connector segments share one Vulkan mesh, and the
overview is fitted uniformly so file tops retain their proportions.
Scanning runs in a worker, includes hidden files, never follows directory symlinks,
and caps each view at 2,500 entries and five levels. A visible notice reports
truncation; open a folder to inspect it further. Directory sizes cover scanned
entries, not an unbounded disk-usage traversal. Directory changes trigger refresh.

| Input | Action |
| --- | --- |
| LMB | Select; information and preview appear in the inspector |
| LMB drag on empty space | Tilt vertically and rotate horizontally |
| Double LMB | Open directory or file in its default application |
| RMB | File actions in the current application surface |
| MMB + motion | Drag/pan the 3D filesystem |
| Short MMB click, then vertical motion | Scroll-button zoom; click, Escape, or leave the view to finish |
| Wheel / Shift+MMB drag | Zoom |
| Alt+LMB drag | Tilt/rotate, including when starting over an object |
| Left / Right cursor keys | Rotate the focused 3D view |
| Up / Down cursor keys | Tilt the focused 3D view, within the drag tilt limits |
| F2 / Delete | Rename / Move to Trash |
| Ctrl+C / Ctrl+X / Ctrl+V | Copy / cut / paste |
| Ctrl+Shift+N / Alt+Up / F5 | New folder / parent / refresh |

With an item selected, wheel, scroll-button and Shift+MMB zoom use its top centre
as the zoom anchor, keeping it at the same screen position even after rotation
or tilt. Without a selection, zoom uses the scene origin. Standalone XR zoom
buttons and two-hand scaling use the same selected-item anchor.

The context menu offers Open, Open With, Cut, Copy, Paste, Copy Location, Rename,
Move to Trash, confirmed permanent deletion, Create Folder, Open Terminal Here
(Konsole), and Properties. Operations currently act on one selected entry. Copy
supports directories and symlinks, refuses destination collisions and copying a
directory into itself, and preserves sources on failure. Cut uses rename; moves
across filesystems report an error. Open With accepts an executable name/path,
not a shell command. Clipboard URLs and KDE cut metadata interoperate with Dolphin.
This is a focused file-operation subset, not full Dolphin feature parity.

The desktop and XR context menus offer **Compress…** for a selected file or folder
(choose a `.zip` or `.tar.gz` name), and **Extract Archive** for `.zip`, `.tar`,
`.tar.gz`, `.tgz`, `.tar.bz2`, `.tbz2`, `.tar.xz`, `.txz`, and `.tar.zst` files.
Archives are created beside the source; extraction creates a sibling folder named
after the archive without its extension. Existing destinations are refused.
Both operations run in a worker and refresh the view when finished. Failed
operations remove temporary output. Hidden files and empty folders are included;
links and special files are rejected. Extraction rejects absolute paths and parent
traversal, and is limited to 100,000 entries and 10 GiB of uncompressed data.
Double-click a supported archive (or use **Browse Archive**) to explore it in
Map, Tree, or Disc. Archive folders support tabs and Up; Up from the archive root
returns to its containing filesystem folder. The location field and Copy Location
use `archive:/absolute/path.zip#folder/member` URLs, with reserved characters
encoded. These URLs also work when passed to fsvr directly. **Open With…** on the
archive itself still launches an external application.

Archive browsing is read-only: rename, delete, clipboard file transfers, drag/drop,
compression and folder creation are unavailable inside archives. Links and special
files are visible but cannot be opened or followed. Directory entries omitted by
an archive are inferred from member paths. Views retain the usual 2,500-entry,
five-level display limit; open a folder to explore deeper. A bounded in-memory
member index avoids repeated archive scans; file changes and F5 revalidate it.
No full extraction is needed to list members. Compressed tar archives can still
require a sequential pass, so very large archives may take time to index.

File cards show MIME/name tiles. Selecting a regular file loads its inspector
preview on demand, and Open/Open With launches a private temporary copy. Copies
are limited to 64 MiB per file and 256 MiB (4,096 files) per application session;
extract the archive normally for larger files. Temporary copies remain available
until fsvr exits; edits to them do not update the archive. Browsing also limits
metadata to 100,000 entries, 16 MiB of names, paths of 4,096 characters / 128
separators, and 10 GiB of declared uncompressed data. Encrypted archives, nested
archive browsing, and modifying archive contents are not supported.

Image formats supported by Qt, text, JSON, XML, valid freedesktop thumbnail caches,
and installed MIME-matching `.thumbnailer` programs supply previews. Install image
format plugins and system thumbnailers for additional formats (PDF, video, office
documents, etc.). Thumbnailers run asynchronously with a timeout and argv expansion,
without a shell. Unknown formats get MIME/name tiles. Previews are textured onto
the top of each file object and displayed at larger size in the inspector.
Object tops also show a separate filename band, including when a thumbnail is
present. Textures match each top's proportions, images retain their aspect ratio,
and text wraps or elides instead of being squeezed to fit. Narrow file tops turn
their cards along the longer axis. Zoom in for small tiles in dense directories.
Folders use gold bodies and name bands, distinct from file colours, and reserve
space for their names below their children.

## DomeD

Launch against the **socket printed by your DomeD compositor**, using the same
`XDG_RUNTIME_DIR`. DomeD owns the OpenXR session in this mode. fsvr uses mouse
and keyboard input exclusively through the normal Wayland seat; it does not
create hand or eye controls or open a second XR session. The companion
integration and deployment steps are in
[docs/domed-integration.md](docs/domed-integration.md).

The compositor publishes a versioned `<wayland-socket>.dome-session.json` record
for each of its two sockets. fsvr verifies the socket peer and compositor PID,
then uses the socket's launch realm, including inherited `WAYLAND_SOCKET` FDs.
DomeD desktop launches retain an opaque desktop view and inline properties;
XR launches use spatial presentation. A missing/invalid record falls back to
desktop presentation. Headset presence alone never selects XR.
`--desktop` explicitly requests desktop presentation while retaining DomeD's
Wayland bridge when needed. `--xr` starts standalone OpenXR only outside DomeD;
inside DomeD it is rejected with instructions to use the ordinary launch.
XR views have a transparent background and omit the desktop base plate. DomeD's
Wayland surface preserves alpha; controls and Places retain their themed panels.
Standalone OpenXR requests passthrough where supported by the active runtime.

```sh
QT_QPA_PLATFORM=wayland WAYLAND_DISPLAY=wayland-N ./build/fsvr --domed /path
```

Use LMB to select files, double-click to open them, RMB for actions, MMB drag
to pan, and the wheel to zoom. A short MMB click enables scroll-button zoom;
move up/down to zoom and click or press Escape to stop. Drag the background or
use arrow keys to tilt the view. DomeD owns window/panel placement.

DomeD's current compositor does not provide the presentation support required by
this client's native Vulkan swapchain. Both DomeD launch realms therefore render the same Qt
Quick scene offscreen with **Vulkan**, reads changed frames back at up to 30 Hz, and presents a
Qt Widgets raster Wayland surface (`wl_shm`). This has CPU/readback overhead and
matches DomeD's current CPU client-texture bridge. Desktop sessions outside DomeD
use native Vulkan presentation.

Idle scenes reuse the last captured frame. Tile textures use 64–512 pixel
resolution tiers as the desktop view zooms, and selected tiles retain the full
512 pixel resolution. The desktop camera culls objects outside its view.

Selecting a file updates the persistent file inspector, including its thumbnail,
MIME type, size, modification time, owner and path. Properties focuses that
inspector. Open and Actions stay pinned below its scrollable details; selection
does not create a separate preview popup.

RMB opens a native Wayland context menu. DomeD owns its placement and oversized
menu scrolling; Qt handles menu keyboard navigation and Escape. fsvr exports the
full menu buffer to avoid nesting Qt's screen-height scroller inside DomeD's
viewport. File operations and confirmation dialogs use the same backend as the
desktop UI.

DomeD and standalone XR modes follow `$XDG_CONFIG_HOME/dome/appearance.json`
(defaulting to `~/.config/dome/appearance.json`). Light/dark colours match DomeD's
shell palette; accent, scene background and panel opacity follow its saved settings.
Changes apply live to QML controls, the Vulkan view, and the inspector,
including when DomeD atomically replaces the settings file. Invalid settings retain
the last valid theme. Outside these XR modes, fsvr uses the host Qt palette.
XR modes use the Fusion Qt Quick Controls style so host-specific styles cannot
override DomeD's palette. No theme settings are written by fsvr.

Optional integration test (Python 3 + Pillow, python-xlib, Xvfb, built DomeD):

```sh
python3 tests/domed.py /path/to/DomeD/target/debug/dome-compositor build/fsvr
```

This starts and stops an isolated nested compositor on a private Xvfb display,
checks main/popup textures, injects LMB/RMB/MMB XR seat events, verifies live theme
changes, idle frame reuse, exact context anchors, native keyboard handling,
oversized menu scrolling, popup dismissal and cleanup. Captures are saved in
`build/domed-probe/`.
It also checks desktop/XR socket and inherited-FD launches while XR is active,
Dome-specific MIME defaults, real FileManager1 requests, Super+E, physical mouse
selection/context menus, MMB drag and hover zoom, wheel zoom, background tilt,
and folder double-clicks. Its private D-Bus session and synthetic XR heartbeat
exercise compositor mouse routing without a headset. It does not modify DomeD
sources, the user's appearance file, or runtime settings.

## Standalone OpenXR

```sh
XR_RUNTIME_JSON=/path/to/runtime.json ./build/fsvr --xr /path
```

Qt Quick 3D XR owns session lifecycle, Vulkan graphics binding, stereo views and
swapchains. A fixed control panel offers navigation, file information/actions,
and zoom controls. Selecting a file updates the control panel inspector; the
inspector remains in the fixed control panel. Tracked-hand joints drive
palm-normalized pinch detection:
index selects, ring opens actions in the fixed panel, middle + motion drags the
filesystem, and simultaneous middle pinches scale it. Aim rays target file objects
and panel controls. A 55 ms debounce, separate press/release thresholds, ambiguous
pinch rejection, stale-frame/jump cancellation, and neutral rearming after tracking
loss protect against unintended clicks. Hand input needs runtime and device
support; lack of hands does not prevent rendering the scene.

## Validation and limits

Core tests cover scan bounds/aggregation/symlink cycles, safe recursive copy,
collisions, layouts, rename validation, clipboard paste, special-character preview
paths, and gesture debounce/hysteresis/tracking loss. Display tests render with
Vulkan and exercise actual picking, context menus, rename dialogs, MMB pan/zoom,
and XR QML component loading. Appearance tests cover startup defaults, atomic live
updates, invalid settings and settings removal. The DomeD test additionally checks
live Wayland popup capture, seat input, and rendered theme changes. Standalone
stereo output and physical hand ergonomics
still require headset testing. The new layouts approximate the original fsv
presentations; legacy color configuration, translations, and animation behavior
have not been ported.

### File drag and drop

Drag a file or folder with the left mouse button into another application, onto
another folder in the 3D view, or onto a place or directory tab. Dropping on the
view background targets the current directory. Native file URLs preserve spaces,
Unicode and special characters in filenames. Drags default to copy; use Shift
for move or Ctrl for copy. Existing destination names and recursive folder drops
are rejected. Moves within fsvr use rename; moves across filesystems report an
error and leave the source intact. Trash items must be restored before dragging.

Desktop and DomeD windows use Qt's native drag-and-drop protocol. Cross-application
drags require drag-and-drop support in the compositor and receiving application.

## Command manuals and related projects

Basic manuals: [fsvr.1](docs/man/fsvr.1). After installation, use `man fsvr`.

Part of the [de-xr desktop](https://github.com/topics/de-xr):
[DomeD](https://github.com/jebot-git/DomeD) provides the desktop/session,
[Ghostty XR](https://github.com/jebot-git/ghostty-xr) the terminal,
[mpvr](https://github.com/jebot-git/mpvr) video playback,
[cmuxr](https://github.com/jebot-git/cmuxr) music playback and an audio-reactive XR floor,
[fsvr](https://github.com/jebot-git/fsv-xr) file browsing,
[qimgvr](https://github.com/jebot-git/qimgvr) image viewing,
[Arc SBS](https://github.com/jebot-git/arc-sbs) depth conversion, and
[LightXR](https://github.com/jebot-git/LightXR) the optional login manager.
Each application has its own dependencies and can be used independently where
its selected backend permits.
