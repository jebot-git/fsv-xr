#!/usr/bin/python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Private pipe protocol for FSVR's native SMB UI; no graphical toolkit or root.

GVfs owns authenticated connections, protocol negotiation and the user-only FUSE
export. This short-lived process isolates slow remote calls from the renderer.
Credentials exist only in the anonymous stdin pipe and GMountOperation.
"""
import json
import os
import re
import signal
import sys
from urllib.parse import quote, unquote, urlsplit, urlunsplit
from gi.repository import Gio, GLib, GObject


def canonical(value):
    if value.startswith('\\\\'):
        value = 'smb:' + value.replace('\\', '/')
    if len(value) > 8192 or re.search(r'%(?![0-9A-Fa-f]{2})', value) or any(ord(c) < 32 or ord(c) == 127 for c in value):
        raise ValueError('Invalid SMB address')
    u = urlsplit(value)
    if (u.scheme.lower() != 'smb' or not u.hostname or '@' in u.netloc or
            u.query or u.fragment or u.port == 0):
        raise ValueError('Use smb://server/share without credentials, query or fragment')
    parts = [unquote(p, errors='strict') for p in u.path.split('/') if p]
    if any(p in ('.', '..') or '/' in p or '\\' in p or
           any(ord(c) < 32 or ord(c) == 127 for c in p) for p in parts):
        raise ValueError('Invalid SMB path component')
    host = '[' + u.hostname + ']' if ':' in u.hostname else u.hostname
    if u.port is not None:
        host += ':' + str(u.port)
    return urlunsplit(('smb', host, '/' + '/'.join(quote(p, safe='') for p in parts), '', ''))


class Client:
    def __init__(self):
        self.loop = GLib.MainLoop()
        self.cancel = Gio.Cancellable()
        self.new_operation()
        self.buffer = b''
        self.started = False
        self.prompt = None
        self.auth_count = 0
        self.mount_retries = 0
        self.finished = False
        self.server_credentials = None
        os.set_blocking(sys.stdin.fileno(), False)
        GLib.io_add_watch(sys.stdin.fileno(), GLib.IO_IN | GLib.IO_HUP | GLib.IO_ERR, self.input)

    def new_operation(self):
        self.operation = Gio.MountOperation()
        self.operation.set_password_save(Gio.PasswordSave.NEVER)
        self.operation.connect('ask-password', self.password)
        self.operation.connect('ask-question', self.question)
        self.operation.connect('show-processes', self.busy)
        self.operation.connect('aborted', lambda *_: self.finish(error='Authentication cancelled'))

    def emit(self, **value):
        print(json.dumps(value, ensure_ascii=True), flush=True)

    def finish(self, **result):
        if not self.finished:
            self.finished = True
            self.emit(event='result', **result)
            self.loop.quit()

    def input(self, fd, condition):
        try:
            chunk = os.read(fd, 32768)
            if not chunk:
                self.cancel.cancel()
                self.loop.quit()
                return False
            self.buffer += chunk
            if len(self.buffer) > 65536:
                raise ValueError('Oversized request')
            while b'\n' in self.buffer:
                line, self.buffer = self.buffer.split(b'\n', 1)
                self.request(json.loads(line))
        except BlockingIOError:
            pass
        except Exception:
            # Do not echo malformed input: it may contain a password.
            self.finish(error='Invalid network request')
            return False
        return not self.finished

    def request(self, request):
        command = request.get('command')
        if command == 'cancel':
            self.cancel.cancel()
            if self.prompt:
                self.operation.reply(Gio.MountOperationResult.ABORTED)
            self.finish(error='Network operation cancelled')
        elif not self.started and command in ('connect', 'disconnect'):
            self.started = True
            self.uri = canonical(request['uri'])
            self.file = Gio.File.new_for_uri(self.uri)
            self.disconnect = command == 'disconnect'
            GLib.timeout_add_seconds(120, self.timeout)
            if self.disconnect:
                self.file.find_enclosing_mount_async(0, self.cancel, self.found_mount)
            elif not urlsplit(self.uri).path.strip('/'):
                # Browsing a server is a different GVfs mount from a share.
                # Supply its account explicitly instead of silently falling
                # back to the desktop username/guest during RPC enumeration.
                self.prompt = 'server-password'
                self.emit(event='password', uri=self.uri, username='', domain='', retry=False,
                          flags=int(Gio.AskPasswordFlags.NEED_USERNAME | Gio.AskPasswordFlags.NEED_DOMAIN |
                                    Gio.AskPasswordFlags.NEED_PASSWORD | Gio.AskPasswordFlags.ANONYMOUS_SUPPORTED))
            else:
                self.file.mount_enclosing_volume(Gio.MountMountFlags.NONE, self.operation,
                                                self.cancel, self.mounted)
        elif self.prompt == 'server-password' and command == 'authenticate':
            self.prompt = None
            self.server_credentials = dict(request)
            uri = urlsplit(self.uri)
            user = str(request.get('username', ''))
            authority = uri.netloc if request.get('anonymous') or not user else quote(user, safe='')+'@'+uri.netloc
            self.file = Gio.File.new_for_uri(urlunsplit((uri.scheme, authority, uri.path, '', '')))
            self.file.mount_enclosing_volume(Gio.MountMountFlags.NONE, self.operation, self.cancel, self.mounted)
            request.clear()
        elif self.prompt == 'password' and command == 'authenticate':
            self.prompt = None
            self.operation.set_anonymous(bool(request.get('anonymous', False)))
            self.operation.set_username(str(request.get('username', ''))[:1024])
            self.operation.set_domain(str(request.get('domain', ''))[:1024])
            self.operation.set_password(str(request.get('password', ''))[:4096])
            self.operation.set_password_save(Gio.PasswordSave.NEVER)
            self.operation.reply(Gio.MountOperationResult.HANDLED)
            # Keep the operation alive until GVfs completes the async request.
            # The helper exits afterwards; no credentials are persisted by FSVR.
            request.clear()
        elif self.prompt == 'question' and command == 'answer':
            choice = request.get('choice', -1)
            self.prompt = None
            if not isinstance(choice, int) or not 0 <= choice < self.choices:
                self.operation.reply(Gio.MountOperationResult.ABORTED)
            else:
                self.operation.set_choice(choice)
                self.operation.reply(Gio.MountOperationResult.HANDLED)
        else:
            raise ValueError('Unexpected request')

    def timeout(self):
        self.cancel.cancel()
        self.finish(error='SMB connection timed out; check the server and retry')
        return False

    def password(self, operation, message, user, domain, flags):
        # The base class's RUN_LAST handler otherwise replies UNHANDLED before
        # an asynchronous native window has time to answer.
        GObject.signal_stop_emission_by_name(operation, 'ask-password')
        if self.server_credentials is not None:
            self.prompt = 'password'
            credentials = self.server_credentials
            self.server_credentials = None
            self.request(credentials)
            return
        self.auth_count += 1
        if self.auth_count > 5:
            operation.reply(Gio.MountOperationResult.ABORTED)
            self.finish(error='Too many authentication attempts')
            return
        self.prompt = 'password'
        self.emit(event='password', uri=self.uri, message=message, username=user or '', domain=domain or '',
                  flags=int(flags), retry=self.auth_count > 1)

    def question(self, operation, message, choices):
        GObject.signal_stop_emission_by_name(operation, 'ask-question')
        self.prompt = 'question'
        self.choices = len(choices)
        self.emit(event='question', message=message, choices=list(choices))

    def busy(self, operation, message, processes, choices):
        GObject.signal_stop_emission_by_name(operation, 'show-processes')
        operation.reply(Gio.MountOperationResult.ABORTED)
        self.finish(error='The share is busy. Close files using it and retry; no processes were stopped.')

    def found_mount(self, file, result):
        try:
            mount = file.find_enclosing_mount_finish(result)
            mount.unmount_with_operation(Gio.MountUnmountFlags.NONE, self.operation,
                                         self.cancel, self.unmounted)
        except GLib.Error as error:
            self.finish(error=error.message)

    def unmounted(self, mount, result):
        try:
            mount.unmount_with_operation_finish(result)
            self.finish(kind='disconnected', uri=self.uri)
        except GLib.Error as error:
            self.finish(error=error.message)

    def mounted(self, file, result):
        if self.finished:
            return
        try:
            file.mount_enclosing_volume_finish(result)
        except GLib.Error as error:
            if not error.matches(Gio.io_error_quark(), Gio.IOErrorEnum.ALREADY_MOUNTED):
                if (0 < self.auth_count < 5 and self.mount_retries < 4 and
                        (error.matches(Gio.io_error_quark(), Gio.IOErrorEnum.PERMISSION_DENIED) or
                         error.matches(Gio.io_error_quark(), Gio.IOErrorEnum.FAILED))):
                    self.mount_retries += 1
                    self.new_operation()
                    self.file.mount_enclosing_volume(Gio.MountMountFlags.NONE, self.operation,
                                                    self.cancel, self.mounted)
                    return
                hint = ' Install the headless gvfs-smb and gvfs-fuse packages.' if error.matches(
                    Gio.io_error_quark(), Gio.IOErrorEnum.NOT_SUPPORTED) else ''
                self.finish(error=error.message + hint)
                return
        try:
            if not urlsplit(self.uri).path.strip('/'):
                self.list_shares()
            else:
                self.export_attempts = 0
                self.export_path()
        except GLib.Error as error:
            self.finish(error=error.message)

    def list_shares(self):
        # Server enumeration is deliberately explicit; no network-wide scan.
        self.file.enumerate_children_async('standard::name,standard::display-name,standard::target-uri',
            Gio.FileQueryInfoFlags.NOFOLLOW_SYMLINKS, GLib.PRIORITY_DEFAULT, self.cancel, self.shares_opened)

    def shares_opened(self, file, result):
        try:
            self.children = file.enumerate_children_finish(result)
            self.share_entries = []
            self.children.next_files_async(64, GLib.PRIORITY_DEFAULT, self.cancel, self.share_batch)
        except GLib.Error as error:
            self.finish(error=error.message)

    def share_batch(self, children, result):
        try:
            batch = children.next_files_finish(result)
            for info in batch:
                target = info.get_attribute_string('standard::target-uri') or self.file.get_child(info.get_name()).get_uri()
                parsed = urlsplit(target)
                # GVfs' SMB browser drops explicit ports in target-uri. Shares
                # belong to this server: retain its validated endpoint and
                # discard any backend account qualification.
                target = urlunsplit(('smb', urlsplit(self.uri).netloc, parsed.path, '', ''))
                self.share_entries.append(dict(name=info.get_display_name(), uri=canonical(target)))
            if not batch or len(self.share_entries) >= 4096:
                children.close_async(GLib.PRIORITY_DEFAULT, None, None)
                self.finish(kind='shares', uri=self.uri, entries=self.share_entries,
                            warning='Share list limited to 4096 entries' if batch else '')
            else:
                children.next_files_async(64, GLib.PRIORITY_DEFAULT, self.cancel, self.share_batch)
        except (GLib.Error, ValueError) as error:
            self.finish(error=str(error))

    def export_path(self):
        if self.finished:
            return False
        try:
            mount = self.file.find_enclosing_mount(self.cancel)
            root = mount.get_root()
            path, root_path = self.file.get_path(), root.get_path()
            if path and root_path:
                parsed = urlsplit(root.get_uri())
                root_uri = canonical(urlunsplit((parsed.scheme, parsed.netloc.rsplit('@', 1)[-1], parsed.path, '', '')))
                self.finish(kind='connected', uri=self.uri, path=path,
                            root_uri=root_uri, root_path=root_path)
                return False
        except GLib.Error as error:
            self.finish(error=error.message)
            return False
        self.export_attempts += 1
        if self.export_attempts >= 30:
            self.finish(error='Share connected, but its filesystem path is unavailable. Install gvfs-fuse and check /dev/fuse access.')
            return False
        GLib.timeout_add(100, self.export_path)
        return False


if __name__ == '__main__':
    signal.pthread_sigmask(signal.SIG_SETMASK, [])
    Client().loop.run()
