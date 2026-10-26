# Native FLTK file workflows

FSVR uses its FLTK/OpenGL interface on both Wayland and X11. DomeD supplies the
universal menu, appearance and XR window surface. File operations run in workers;
the interface remains responsive and the status area offers progress and Cancel.

## Selection and Places

The file list supports Ctrl-click, Shift-click and Ctrl+A. A spatial click focuses
the corresponding list row; Ctrl-click in the scene toggles it. Drag an already
selected row to carry the whole selection. Shift-left drag still pans the scene.

Places contains Home, configured XDG directories, Trash and File System, plus
local bookmarks from GTK (`gtk-3.0/bookmarks` and `.gtk-bookmarks`) and Dolphin
(`user-places.xbel`). SMB entries are supported; other remote entries are omitted. Ctrl+D or Go → Add bookmark saves
the current directory in `$XDG_CONFIG_HOME/fsvr/places`; Remove bookmark removes
only an FSVR bookmark. Imported bookmark files are never rewritten.

## Trash

Go → Trash, the Places row or `fsvr trash:///` shows the current user's home and
mounted-volume Trash stores. The inspector includes the original location and
deletion date when metadata is available. Files moved to Trash use GIO's Trash
implementation. Restore from Trash restores the selection to its original parent
directories; a missing parent or an existing destination reports an error.

Delete permanently deletes selected Trash entries; Empty Trash empties the
discovered user stores. Both ask for confirmation. The displayed scan is bounded
to 8,192 entries; Empty Trash also handles items beyond that view limit. Damaged
metadata can make restoration unavailable while leaving permanent deletion usable.
Trash contents cannot be opened as ordinary writable files, pasted into, renamed,
or exported through file dragging. Restore before opening an item.

## Clipboard, dragging and transfers

Ctrl+C/X/V uses `text/uri-list`, GNOME copied-files and KDE cut-selection formats.
Unicode, spaces, percent signs and newlines in filenames survive URI encoding.
Incoming lists are limited to 4,096 paths; file lists are limited to 1 MiB. Clipboard owners must answer within
four seconds. FSVR must remain running to serve its clipboard selection.

Wayland uses the compositor's core seat data-device protocol with the existing
FLTK display; X11 uses CLIPBOARD/XDND, including incremental transfers. No GUI
toolkit clipboard helpers or clipboard shell commands are required at runtime.

Left-drag from the file list or spatial scene exports selected local files.
Dragging negotiates **Copy** only, including when the originating clipboard
selection was Cut. Drop on a spatial folder to copy into it, or on the current
file list/empty scene area to copy into the current folder. Other UI areas do not
accept file drops. Use Cut/Paste for moves.

Conflicts offer Cancel, Keep both or Skip; the chosen policy applies to subsequent
conflicts in that operation. Keep both creates numbered names. There is no overwrite
mode. Selecting a directory and its child transfers that child once. Copies recurse
through directories, include hidden files and preserve symlinks without traversing
them; special files are refused. Copying a directory into itself is refused.

Same-filesystem moves use an atomic, non-overwriting rename. Cross-filesystem moves
copy first and verify the source identity and timestamps before removing it.
Changes detected during a move retain the source and report an error. A late source
removal failure may leave both copies and requires inspecting the reported item.
Cancellation rolls back the current incomplete copy; previously completed items
remain. Completed move sources are removed from FSVR's cut clipboard even if a
later item fails or the operation is cancelled.

## Current limits and verification

Local paths, local `file://` URLs and SMB URLs are accepted. SMB server/share
browsing, native authentication, stable bookmarks and normal file operations use
the headless GVfs SMB/FUSE runtime; see [SMB support](SMB.md). Other remote schemes
are not implemented. Trash directory
contents are restored as units rather than browsed in place. ACLs, extended
attributes, hard-link relationships and sparse allocation are not guaranteed to
survive copies. Fractional scaling and physical-headset input need broader coverage.

Core/UI regression tests run with `ctest --test-dir build-fltk --output-on-failure`.
Additional cross-process protocol tests require Xvfb, Python Xlib, `wl-copy` and an
installed DomeD compositor with its local preview API:

```sh
uv run --no-project --with python-xlib python tests/file-transfers-x11.py
uv run --no-project --with python-xlib python tests/file-transfers-wayland.py
FSVR_WAYLAND_TEST_BINARY="$PWD/build-fltk/fltk/fsv-desktop-ui-test" \
  python3 tests/gltf/test-wayland.py
```

The protocol tests create private display/session fixtures and temporary files.
They check native file MIME exchange, external clipboard ownership, real pointer
dragging, X11 incremental payloads and copying into FSVR. Core tests cover Trash
restoration/collisions/stale metadata, explicit isolated-store emptying, bookmarks,
recursive copies, cancellation, partial completion and real cross-filesystem moves
when `/dev/shm` is a different filesystem. They do not empty the user's actual Trash.
