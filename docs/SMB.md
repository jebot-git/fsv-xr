# SMB / Samba shares

The default FLTK frontend supports SMB servers and shares through the headless
GIO/GVfs SMB backend. It does not start a Samba server or require a root mount.
Install `gvfs`, `gvfs-client`, `gvfs-smb`, `gvfs-fuse`, `python3` and
`python3-gobject-base` (Fedora/Nobara package names). The login requires a session
bus and access to `/dev/fuse`.

Choose **Go → Connect to SMB server…**, enter `smb://server/` to list its shares,
or open `smb://server/share/folder` directly in the location field or command
line. UNC addresses such as `\\server\share` are also accepted. Server discovery
is explicit; opening FSVR does not scan the network. IPv6 literals and optional
ports are accepted. Use percent encoding for reserved characters in URL paths.

Authentication uses native FLTK username/domain and masked-password controls,
with guest access when the server allows it. Credentials travel over private
process pipes and GMountOperation, never command arguments, bookmarks or logs.
FSVR requests no permanent password storage; GVfs may reuse an existing login
connection. Incorrect passwords can be retried. Cancel stops the pending helper;
connection attempts have a two-minute deadline. No Qt or GTK frontend is used.

Connected shares appear in the existing list and spatial views. Their session's
user-only GVfs FUSE paths supply existing open, preview, archive, new-folder,
rename, copy/cut/paste, drag-and-drop and progress/cancellation workflows.
Network scans enumerate the current folder rather than recursively walking a
server tree. Transfer conflicts retain the existing Keep both / Skip / Cancel
policy. Server permissions and filesystem semantics still apply. Trash is only
available where the backend supports it; failed trash requests never fall back
to permanent deletion. Existing transfer metadata limitations still apply.

**Go → Add bookmark** saves the stable SMB URL, not the temporary FUSE path.
SMB bookmarks imported from GTK and Dolphin are shown in Places. Reopening one
reconnects and can ask for credentials. Up from a share root lists its server's
shares. **Go → Disconnect SMB share** requests a normal unmount for this login;
busy-share failures are reported without forcing unmount or terminating apps.

The GVfs connection is shared with other applications in the login. Closing FSVR
does not unmount a share. Disconnecting can affect other applications using it.
An SMB server alone is not a local filesystem: if GVfs connects but cannot
export a FUSE path, FSVR reports the missing capability instead of treating the
URL as a local path. SMB dialect/authentication policy follows the distribution's
Samba client configuration; FSVR does not enable obsolete protocols.

## Integration checks

The normal CTest suite covers URL validation and stable SMB bookmarks. For real
authentication, share enumeration, permission enforcement and transfers, build
the disposable loopback-only Samba fixture and run:

```sh
podman build -t localhost/fsvr-smb-test:latest -f tests/smb/Containerfile tests/smb
python3 tests/smb/integration.py
python3 tests/smb/integration.py --ui build-fltk/fltk/fsv-network-ui-test
python3 tests/smb/integration.py --ui build-fltk/fltk/fsv-network-ui-test --wayland
```

These require rootless Podman, the runtime packages above, and `/dev/fuse`.
UI checks additionally use Xvfb; the Wayland check runs an isolated nested
`dome-compositor` and uses libXtst for a private input serial. They never use
the active desktop's clipboard, mounts, devices or saved network credentials.
