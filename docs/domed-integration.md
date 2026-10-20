# DomeD default file-manager integration

Use the ordinary **fsvr** desktop entry in both DomeD realms. The standalone
OpenXR entry creates its own XR session and is for use outside DomeD.

## Install

Build/install fsvr, then run:

```sh
cmake --install build --prefix "$HOME/.local"
update-desktop-database "$HOME/.local/share/applications"
"$HOME/.local/bin/fsvr-dome-default"
```

The helper updates only `$XDG_CONFIG_HOME/dome-mimeapps.list` (default
`~/.config/dome-mimeapps.list`). It sets `inode/directory` and
`x-scheme-handler/trash` to `org.fsvxr.fsvr.desktop`, preserving other Dome
preferences. Desktop-specific defaults are selected by `XDG_CURRENT_DESKTOP=Dome`;
KDE/GNOME and global `mimeapps.list` files are untouched. A distribution-level
`dome-mimeapps.list` is also installed beside the application entry.

The companion DomeD support is published in
[commit 9657b60](https://github.com/jebot-git/DomeD/commit/9657b600a54eb671338e24988c1585d618f92eca).
Use that revision or later. For older checkouts, the same changes are supplied
as [a patch](../integration/domed-file-manager.patch).
They add **Open Files** / **Super+E**, a session-local FileManager1 service and
per-socket realm metadata. Apply against the DomeD revision recorded in the patch
header, then rebuild/install DomeD using its own build and installation scripts.
Restart DomeD to use the new compositor; updating fsvr alone cannot update an
already running compositor.

`DOME_FILE_MANAGER` can override the file-manager executable (one executable
path, not a shell command). The default is `fsvr`, which must be on DomeD's PATH.
DomeD launches it on the selected realm's socket and removes an inherited
`WAYLAND_SOCKET` to avoid reconnecting to the wrong realm.

## Launch contract

DomeD writes `<absolute-wayland-socket-path>.dome-session.json` with mode 0600:

```json
{"version":1,"compositor_pid":1234,"realm":"xr"}
```

The other realm is `desktop`. Records exist for both listeners and are removed
on normal compositor shutdown. Clients identify the actual same-user
`dome-compositor` peer with `SO_PEERCRED` and match the record's PID. A stale,
unsupported, missing, linked or publicly writable record never enables XR.
`getpeername()` identifies the socket even when Qt receives an inherited FD.
Headset visibility, mouse ownership and socket numbering are not launch hints.
Older DomeD versions without records use desktop presentation until upgraded.

Both realms use Vulkan offscreen rendering and a raster Wayland surface because
DomeD currently consumes `wl_shm` buffers. Desktop presentation has an opaque
scene and inline properties; XR presentation has transparency and spatial popups.
The realm remains fixed for that application instance across headset reconnects.
`--desktop` overrides presentation. `--domed` explicitly enables the bridge while
keeping the socket's realm. Both realms use mouse and keyboard input exclusively,
with no fsvr hand or eye controls. `--xr` is rejected inside DomeD; standalone
OpenXR is available outside DomeD.

## File-manager requests

The desktop entry accepts multiple local paths/file URLs and `trash:/`; additional
locations open in tabs. `--select` reveals items in their parent folders;
`--properties` also opens their properties after scanning and view initialization.
Non-local URLs are rejected rather than sent back to the default file manager.

In a native Dome login, the compositor owns `org.freedesktop.FileManager1` at
`/org/freedesktop/FileManager1` and implements `ShowFolders`, `ShowItems` and
`ShowItemProperties`. It prefers the requesting process's named Wayland socket,
falling back to the active input realm when the caller has no identifiable socket.
This keeps background desktop callers on the desktop while XR is active.
An existing FileManager1 owner is never replaced. Nested development sessions
leave the host service alone; tests can opt in with `DOME_FILE_MANAGER_SERVICE=1`
on a private bus. Set it to `0` to disable the service.

## Mouse verification

```sh
ctest --test-dir build --output-on-failure
python3 tests/domed.py /path/to/DomeD/target/debug/dome-compositor build/fsvr
```

The compositor test uses private Xvfb/D-Bus sessions and a synthetic XR heartbeat.
Actual X mouse events enter DomeD's physical XR mouse route: LMB selection and
background tilt, RMB menus, MMB drag and scroll-button zoom, wheel zoom, and
folder double-click. It also checks MIME defaults, FileManager1 requests,
Super+E, inherited socket FDs and desktop launches while XR owns input.
This is an automated compositor/input test, not a physical-headset test.
