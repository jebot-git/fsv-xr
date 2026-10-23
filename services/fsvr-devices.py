#!/usr/bin/python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""FSVR removable-media service. Only Python, GIO and UDisks2; no GUI imports."""
import json
import os
from pathlib import Path
import signal
import tempfile
from gi.repository import Gio, GLib

UDISKS = 'org.freedesktop.UDisks2'
ROOT = '/org/freedesktop/UDisks2'
NAME = 'org.fsvxr.Devices'
PATH = '/org/fsvxr/Devices'
XML = '''<node><interface name="org.fsvxr.Devices">
<method name="Snapshot"><arg type="s" direction="out"/></method>
<method name="SetAutomount"><arg type="b" direction="in"/></method>
<method name="Action"><arg name="action" type="s" direction="in"/>
<arg name="device" type="s" direction="in"/><arg name="secret" type="s" direction="in"/>
<arg type="s" direction="out"/></method></interface></node>'''


def text(value):
    if isinstance(value, (bytes, bytearray, list)):
        return bytes(value).rstrip(b'\0').decode('utf-8', 'replace')
    return str(value or '')


def devices(objects):
    """Derive rows from UDisks' typed cache; never infer trust from device names."""
    result = []
    for path, interfaces in objects.items():
        block = interfaces.get('Block')
        if not block or block.get('HintIgnore', False):
            continue
        filesystem = interfaces.get('Filesystem')
        encrypted = interfaces.get('Encrypted')
        if filesystem is None and encrypted is None:
            continue
        backing = block.get('CryptoBackingDevice', '/')
        drive = block.get('Drive', '/')
        if drive == '/' and backing != '/':
            drive = objects.get(backing, {}).get('Block', {}).get('Drive', '/')
        props = objects.get(drive, {}).get('Drive', {})
        external = bool(props.get('Removable') or props.get('MediaRemovable') or
                        props.get('ConnectionBus') in ('usb', 'sdio', 'ieee1394'))
        system = block.get('HintSystem', True)
        mounts = [text(m) for m in (filesystem or {}).get('MountPoints', [])]
        mounts = [m for m in mounts if m.startswith('/')]
        clear = next((p for p, obj in objects.items()
                      if obj.get('Block', {}).get('CryptoBackingDevice') == path), '')
        # The unlocked filesystem represents an encrypted volume without a second row.
        if encrypted is not None and clear:
            continue
        if not external and not mounts:
            continue
        device = text(block.get('PreferredDevice') or block.get('Device'))
        label = block.get('HintName') or block.get('IdLabel') or os.path.basename(device) or 'Volume'
        result.append(dict(id=path, name=label, device=device, drive=drive,
                           mounts=mounts, size=block.get('Size', 0),
                           filesystem=filesystem is not None, locked=encrypted is not None,
                           backing=backing if backing != '/' else '',
                           removable=external and not system,
                           ejectable=bool(props.get('Ejectable')),
                           poweroff=bool(props.get('CanPowerOff')),
                           # Respect fstab/crypttab ownership and udev hint policy.
                           automount=(external and not system and filesystem is not None
                                      and block.get('IdUsage') == 'filesystem'
                                      and block.get('HintAuto', False)
                                      and not block.get('Configuration')
                                      and 'Loop' not in interfaces)))
    return sorted(result, key=lambda row: (not row['removable'], row['name'].casefold(), row['id']))


def drive_group(objects, drive):
    sibling = objects.get(drive, {}).get('Drive', {}).get('SiblingId')
    return {drive} | {path for path, obj in objects.items()
                      if sibling and obj.get('Drive', {}).get('SiblingId') == sibling}


def removable_group(objects, group):
    for obj in objects.values():
        block = obj.get('Block', {})
        if block.get('Drive') not in group:
            continue
        mounts = obj.get('Filesystem', {}).get('MountPoints', [])
        if block.get('HintSystem', True) or (block.get('HintIgnore') and mounts):
            return False
        if any(text(m) in ('/', '/boot', '/boot/efi', '/home', '/usr', '/var') for m in mounts):
            return False
    return True


class Service:
    def __init__(self):
        self.loop = GLib.MainLoop()
        self.manager = None
        self.connection = None
        self.system = None
        self.objects = {}
        self.busy = set()
        self.seen = set()
        self.suppressed = set()
        self.errors = {}
        self.error = ''
        self.config = Path(os.environ.get('XDG_CONFIG_HOME', str(Path.home()/'.config'))) / 'fsvr/automount.json'
        self.enabled = True
        try:
            saved = json.loads(self.config.read_text())
            self.enabled = saved.get('enabled', True) is True
        except (OSError, ValueError):
            pass

    def save(self, enabled):
        self.config.parent.mkdir(parents=True, exist_ok=True)
        fd, path = tempfile.mkstemp(prefix='.automount-', dir=self.config.parent)
        try:
            with os.fdopen(fd, 'w') as stream:
                json.dump({'enabled': enabled}, stream)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(path, self.config)
        finally:
            if os.path.exists(path):
                os.unlink(path)
        self.enabled = enabled
        # Enabling explicitly retries currently present eligible volumes.
        if enabled:
            self.seen.clear()
            self.suppressed.clear()
        self.refresh()

    def connect_udisks(self):
        if self.manager is not None:
            return GLib.SOURCE_REMOVE
        try:
            self.system = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
            self.manager = Gio.DBusObjectManagerClient.new_sync(
                self.system, Gio.DBusObjectManagerClientFlags.NONE, UDISKS, ROOT,
                None, None, None)
            for name in ('object-added', 'object-removed', 'interface-added', 'interface-removed',
                         'interface-proxy-properties-changed'):
                self.manager.connect(name, self.changed)
            self.manager.connect('notify::name-owner', self.changed)
            self.refresh()
            return GLib.SOURCE_REMOVE
        except GLib.Error:
            self.error = 'UDisks2 is unavailable; retrying.'
            return GLib.SOURCE_CONTINUE

    def changed(self, *_):
        self.refresh()

    def refresh(self):
        objects = {}
        if self.manager is not None:
            for obj in self.manager.get_objects():
                interfaces = {}
                for interface in obj.get_interfaces():
                    name = interface.get_interface_name().removeprefix(UDISKS + '.')
                    interfaces[name] = {p: interface.get_cached_property(p).unpack()
                                        for p in interface.get_cached_property_names() or []}
                objects[obj.get_object_path()] = interfaces
            self.error = '' if self.manager.get_name_owner() else 'UDisks2 is unavailable; waiting for it to return.'
        self.objects = objects
        self.seen.intersection_update(objects)
        self.suppressed.intersection_update(objects)
        self.errors = {key: value for key, value in self.errors.items() if key in objects}
        for row in devices(objects):
            if not row['automount'] or not self.enabled or row['id'] in self.seen:
                continue
            # Mark once per insertion, including already mounted media: an explicit
            # unmount must not trigger an immediate remount, even in another client.
            if row['drive'] in self.busy:
                continue
            self.seen.add(row['id'])
            if row['mounts'] or row['id'] in self.suppressed:
                continue
            self.action('mount', row['id'], '', None, automatic=True)

    def call(self, path, interface, method, parameters, done, automatic=False):
        flags = Gio.DBusCallFlags.NONE if automatic else Gio.DBusCallFlags.ALLOW_INTERACTIVE_AUTHORIZATION
        self.system.call(UDISKS, path, UDISKS+'.'+interface, method, parameters,
                         None, flags, 120000, None, self.finished, done)

    @staticmethod
    def finished(connection, response, done):
        try:
            reply = connection.call_finish(response)
            done(reply.unpack(), None)
        except GLib.Error as error:
            done(None, error.message)

    def action(self, action, identity, secret, invocation, automatic=False):
        row = next((r for r in devices(self.objects) if r['id'] == identity), None)
        def reject(message):
            if invocation:
                invocation.return_dbus_error(NAME+'.Error', message)
        if row is None:
            reject('This device is no longer available.')
            return
        if not row['removable']:
            reject('System and fixed volumes are browse-only.')
            return
        group = drive_group(self.objects, row['drive'])
        if group & self.busy:
            reject('A device operation is already in progress.')
            return
        if action not in ('mount', 'unmount', 'unlock', 'remove', 'lock'):
            reject('Unknown device operation.')
            return
        if action == 'mount' and row['mounts']:
            if invocation:
                invocation.return_value(GLib.Variant('(s)', (json.dumps({'path':row['mounts'][0]}),)))
            return
        if action == 'mount' and not row['filesystem'] or action == 'unlock' and not row['locked']:
            reject('The device does not support this operation.')
            return
        if action == 'lock' and not row['backing']:
            reject('This filesystem is not an unlocked encrypted volume.')
            return
        if action == 'remove' and not (row['poweroff'] or row['ejectable']):
            reject('This drive supports unmounting only.')
            return
        affected = [r for r in devices(self.objects) if r['drive'] in group]
        # A mixed system/removable drive must never be powered off by this service.
        if action == 'remove' and (not removable_group(self.objects, group) or any(not r['removable'] for r in affected)):
            reject('The drive contains a system volume.')
            return
        self.busy.update(group)
        self.errors.pop(identity, None)
        opts = {'auth.no_user_interaction': GLib.Variant('b', automatic)}
        steps = []
        if action in ('unmount', 'lock', 'remove'):
            targets = affected if action == 'remove' else [row]
            for target in targets:
                self.suppressed.add(target['id'])
                self.seen.add(target['id'])
                if target['mounts']:
                    steps.append((target['id'], 'Filesystem', 'Unmount', GLib.Variant('(a{sv})', (opts,))))
            if action == 'lock':
                steps.append((row['backing'], 'Encrypted', 'Lock', GLib.Variant('(a{sv})', (opts,))))
            if action == 'remove':
                for backing in sorted({r['backing'] for r in targets if r['backing']}):
                    steps.append((backing, 'Encrypted', 'Lock', GLib.Variant('(a{sv})', (opts,))))
                steps.append((row['drive'], 'Drive', 'PowerOff' if row['poweroff'] else 'Eject', GLib.Variant('(a{sv})', (opts,))))
        elif action == 'mount':
            steps.append((identity, 'Filesystem', 'Mount', GLib.Variant('(a{sv})', (opts,))))
        else:
            steps.append((identity, 'Encrypted', 'Unlock', GLib.Variant('(sa{sv})', (secret, opts))))
        secret = ''  # Never persist, log, or place passphrases in process arguments.
        def advance(result=None, error=None):
            if error or not steps:
                self.busy.difference_update(group)
                if error:
                    # UDisks errors contain context but no passphrase; do not log requests.
                    self.errors[identity] = error
                if invocation:
                    if error:
                        invocation.return_dbus_error(NAME+'.Error', error)
                    else:
                        path = result[0] if action == 'mount' and result else ''
                        invocation.return_value(GLib.Variant('(s)', (json.dumps({'path':path}),)))
                self.refresh()
                return
            path, interface, method, parameters = steps.pop(0)
            self.call(path, interface, method, parameters, advance, automatic)
        advance()

    def method(self, connection, sender, path, interface, method, params, invocation):
        try:
            if method == 'Snapshot':
                rows = devices(self.objects)
                for row in rows:
                    row['busy'] = row['drive'] in self.busy
                    row['error'] = self.errors.get(row['id'], '')
                snapshot = dict(enabled=self.enabled, error=self.error, devices=rows)
                invocation.return_value(GLib.Variant('(s)', (json.dumps(snapshot),)))
            elif method == 'SetAutomount':
                self.save(params.unpack()[0])
                invocation.return_value(None)
            elif method == 'Action':
                self.action(*params.unpack(), invocation)
        except (OSError, ValueError, GLib.Error) as error:
            invocation.return_dbus_error(NAME+'.Error', str(error))

    def acquired(self, connection, _):
        self.connection = connection
        info = Gio.DBusNodeInfo.new_for_xml(XML)
        connection.register_object(PATH, info.interfaces[0], self.method, None, None)
        self.connect_udisks()
        GLib.timeout_add_seconds(5, self.connect_udisks)

    def run(self):
        owner = Gio.bus_own_name(Gio.BusType.SESSION, NAME, Gio.BusNameOwnerFlags.DO_NOT_QUEUE,
                                self.acquired, None, lambda *_: self.loop.quit())
        for sig in (signal.SIGTERM, signal.SIGINT):
            GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, sig, lambda: (self.loop.quit(), False)[1])
        self.loop.run()
        Gio.bus_unown_name(owner)

if __name__ == '__main__':
    if '--version' in __import__('sys').argv:
        print('fsvr-devices 0.1 (GIO/UDisks2, no graphical toolkit)')
    else:
        Service().run()
