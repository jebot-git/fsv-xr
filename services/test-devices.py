#!/usr/bin/python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Policy checks plus real D-Bus IPC against an isolated fake UDisks2 service."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from gi.repository import Gio, GLib

HERE = Path(__file__).parent
spec = importlib.util.spec_from_file_location('devices', HERE/'fsvr-devices.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
U = module.UDISKS
R = module.ROOT
D = R+'/drives/usb'
B = R+'/block_devices/sdz1'

def fixture():
    return {D:{'Drive':dict(Removable=True, MediaRemovable=True, ConnectionBus='usb',
                           CanPowerOff=True, Ejectable=False)},
            B:{'Block':dict(Drive=D, IdLabel='USB data', Device=b'/dev/sdz1\0',
                           Size=4096, HintSystem=False, HintIgnore=False, HintAuto=True,
                           IdUsage='filesystem', Configuration=[], CryptoBackingDevice='/'),
               'Filesystem':{'MountPoints':[]}}}

class Policy(unittest.TestCase):
    def test_removable_mountable(self):
        row=module.devices(fixture())[0]
        self.assertTrue(row['automount']);self.assertTrue(row['removable'])
        self.assertEqual(row['device'],'/dev/sdz1')
    def test_ignore_system_and_hint(self):
        for key,value in [('HintSystem',True),('HintAuto',False),('IdUsage','raid'),('Configuration',[('fstab',{})])]:
            objects=fixture();objects[B]['Block'][key]=value
            self.assertFalse(module.devices(objects)[0]['automount'],key)
        objects=fixture();objects[B]['Block']['HintIgnore']=True
        self.assertEqual(module.devices(objects),[])
    def test_internal_and_loop(self):
        objects=fixture();objects[D]['Drive']=dict(Removable=False,ConnectionBus='ata')
        self.assertEqual(module.devices(objects),[])
        objects[B]['Filesystem']['MountPoints']=[b'/\0']
        self.assertFalse(module.devices(objects)[0]['automount'])
        objects=fixture();objects[B]['Loop']={'BackingFile':b'/tmp/disk'}
        self.assertFalse(module.devices(objects)[0]['automount'])
    def test_external_ssd(self):
        objects=fixture();objects[D]['Drive']['Removable']=False;objects[D]['Drive']['MediaRemovable']=False
        self.assertTrue(module.devices(objects)[0]['automount'])
    def test_crypto_backing(self):
        objects=fixture();objects[B].pop('Filesystem');objects[B]['Encrypted']={};objects[B]['Block']['IdUsage']='crypto'
        self.assertTrue(module.devices(objects)[0]['locked'])
        clear=R+'/block_devices/dm_2d0';objects[clear]=fixture()[B]
        objects[clear]['Block']['Drive']='/';objects[clear]['Block']['CryptoBackingDevice']=B
        rows=module.devices(objects)
        self.assertEqual(len(rows),1);self.assertEqual(rows[0]['id'],clear)
        self.assertEqual(rows[0]['drive'],D);self.assertEqual(rows[0]['backing'],B)
    def test_byte_paths(self):
        objects=fixture();objects[B]['Filesystem']['MountPoints']=[list('/run/media/me/日本\0'.encode())]
        self.assertEqual(module.devices(objects)[0]['mounts'],['/run/media/me/日本'])
    def test_sibling_and_system_drive_removal(self):
        objects=fixture();other=R+'/drives/other'
        objects[D]['Drive']['SiblingId']='shared-usb'
        objects[other]={'Drive':{'SiblingId':'shared-usb'}}
        group=module.drive_group(objects,D)
        self.assertEqual(group,{D,other});self.assertTrue(module.removable_group(objects,group))
        objects[R+'/block_devices/system']={'Block':{'Drive':other,'HintSystem':True}}
        self.assertFalse(module.removable_group(objects,group))
        objects.pop(R+'/block_devices/system');objects[B]['Filesystem']['MountPoints']=[b'/boot\0']
        self.assertFalse(module.removable_group(objects,group))
    def test_no_graphical_imports(self):
        self.assertNotIn('gi.repository.Gtk',sys.modules)
        self.assertNotIn('gi.repository.Gdk',sys.modules)

TYPES={'Removable':'b','MediaRemovable':'b','CanPowerOff':'b','Ejectable':'b','ConnectionBus':'s',
       'Drive':'o','IdLabel':'s','Device':'ay','Size':'t','HintSystem':'b','HintIgnore':'b','HintAuto':'b',
       'SiblingId':'s','IdUsage':'s','Configuration':'a(sa{sv})','CryptoBackingDevice':'o','MountPoints':'aay'}
def variants(props):
    return {k:GLib.Variant(TYPES[k],v) for k,v in props.items()}

def fake():
    assert os.environ['DBUS_SYSTEM_BUS_ADDRESS']==os.environ['DBUS_SESSION_BUS_ADDRESS']
    conn=Gio.bus_get_sync(Gio.BusType.SESSION,None)
    objects=fixture();calls=[];busy=False
    rootxml='''<node><interface name="org.freedesktop.DBus.ObjectManager">
    <method name="GetManagedObjects"><arg type="a{oa{sa{sv}}}" direction="out"/></method>
    <signal name="InterfacesAdded"><arg type="o"/><arg type="a{sa{sv}}"/></signal>
    <signal name="InterfacesRemoved"><arg type="o"/><arg type="as"/></signal>
    </interface><interface name="org.fsvxr.TestBackend">
    <method name="Calls"><arg type="s" direction="out"/></method>
    <method name="Busy"><arg type="b" direction="in"/></method>
    <method name="Add"><arg type="s" direction="in"/></method>
    <method name="AddEncrypted"/><method name="Quit"/></interface></node>'''
    xml=Gio.DBusNodeInfo.new_for_xml(rootxml)
    def encode(obj):return {U+'.'+name:variants(props) for name,props in obj.items()}
    def signal_props(path,interface,props):
        conn.emit_signal(None,path,'org.freedesktop.DBus.Properties','PropertiesChanged',
                         GLib.Variant('(sa{sv}as)',(U+'.'+interface,variants(props),[])))
    def method(c,sender,path,interface,name,params,inv):
        nonlocal busy
        if name=='GetManagedObjects':
            inv.return_value(GLib.Variant('(a{oa{sa{sv}}})',({p:encode(obj) for p,obj in objects.items()},)))
        elif name=='Calls':inv.return_value(GLib.Variant('(s)',(json.dumps(calls),)))
        elif name=='Busy':busy=params.unpack()[0];inv.return_value(None)
        elif name=='Add':
            new=params.unpack()[0];objects[new]=fixture()[B];register(new,objects[new]);
            conn.emit_signal(None,R,'org.freedesktop.DBus.ObjectManager','InterfacesAdded',GLib.Variant('(oa{sa{sv}})',(new,encode(objects[new]))));inv.return_value(None)
        elif name=='AddEncrypted':
            new=R+'/block_devices/luks';objects[new]=fixture()[B]
            objects[new].pop('Filesystem');objects[new]['Encrypted']={}
            objects[new]['Block']['IdUsage']='crypto'
            register(new,objects[new])
            conn.emit_signal(None,R,'org.freedesktop.DBus.ObjectManager','InterfacesAdded',GLib.Variant('(oa{sa{sv}})',(new,encode(objects[new]))));inv.return_value(None)
        elif name=='Quit':inv.return_value(None);loop.quit()
        elif name in ('Mount','Unmount','PowerOff','Unlock','Lock'):
            args=params.unpack()
            calls.append([name,path,args[-1]])
            if name=='Unmount' and busy:
                inv.return_dbus_error(U+'.Error.DeviceBusy','Device is busy');return
            if name=='Mount':
                point=os.environ['FSVR_TEST_MOUNT']
                objects[path]['Filesystem']['MountPoints']=[point.encode()+b'\0']
                signal_props(path,'Filesystem',objects[path]['Filesystem'])
                inv.return_value(GLib.Variant('(s)',(point,)))
            elif name=='Unmount':
                objects[path]['Filesystem']['MountPoints']=[]
                signal_props(path,'Filesystem',objects[path]['Filesystem']);inv.return_value(None)
            elif name=='PowerOff':
                for p in list(objects):
                    if p==D or objects[p].get('Block',{}).get('Drive')==D:
                        interfaces=list(encode(objects.pop(p)))
                        conn.emit_signal(None,R,'org.freedesktop.DBus.ObjectManager','InterfacesRemoved',GLib.Variant('(oas)',(p,interfaces)))
                inv.return_value(None)
            elif name=='Unlock':
                assert args[0]=='fixture-passphrase'
                clear=R+'/block_devices/clear';objects[clear]=fixture()[B]
                objects[clear]['Block']['CryptoBackingDevice']=path
                register(clear,objects[clear])
                conn.emit_signal(None,R,'org.freedesktop.DBus.ObjectManager','InterfacesAdded',GLib.Variant('(oa{sa{sv}})',(clear,encode(objects[clear]))))
                inv.return_value(GLib.Variant('(o)',(clear,)))
            elif name=='Lock':
                clear=R+'/block_devices/clear'
                interfaces=list(encode(objects.pop(clear)))
                conn.emit_signal(None,R,'org.freedesktop.DBus.ObjectManager','InterfacesRemoved',GLib.Variant('(oas)',(clear,interfaces)))
                inv.return_value(None)
            else:inv.return_value(None)
    def register(path,obj):
        for interface,props in obj.items():
            methods=''
            if interface=='Filesystem':methods='<method name="Mount"><arg type="a{sv}" direction="in"/><arg type="s" direction="out"/></method><method name="Unmount"><arg type="a{sv}" direction="in"/></method>'
            if interface=='Encrypted':methods='<method name="Unlock"><arg type="s" direction="in"/><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method><method name="Lock"><arg type="a{sv}" direction="in"/></method>'
            if interface=='Drive':methods='<method name="PowerOff"><arg type="a{sv}" direction="in"/></method>'
            properties=''.join(f'<property name="{key}" type="{TYPES[key]}" access="read"/>' for key in props)
            info=Gio.DBusNodeInfo.new_for_xml(f'<node><interface name="{U}.{interface}">{properties}{methods}</interface></node>')
            conn.register_object(path,info.interfaces[0],method,lambda c,s,p,i,k:variants(objects[p][i.removeprefix(U+'.')])[k],None)
    for info in xml.interfaces:conn.register_object(R,info,method,None,None)
    for path,obj in objects.items():register(path,obj)
    loop=GLib.MainLoop()
    owner=Gio.bus_own_name_on_connection(conn,U,Gio.BusNameOwnerFlags.NONE,None,None)
    loop.run();Gio.bus_unown_name(owner)


def integration():
    os.environ['DBUS_SYSTEM_BUS_ADDRESS']=os.environ['DBUS_SESSION_BUS_ADDRESS']
    with tempfile.TemporaryDirectory() as temp:
        os.environ['XDG_CONFIG_HOME']=temp
        os.environ['FSVR_TEST_MOUNT']=temp+'/mounted'
        Path(temp+'/mounted').mkdir()
        fake_process=subprocess.Popen([sys.executable,__file__,'--fake'])
        service=None
        connection=Gio.bus_get_sync(Gio.BusType.SESSION,None)
        def call(name,path,interface,method,params=None):
            return connection.call_sync(name,path,interface,method,params,None,Gio.DBusCallFlags.NONE,5000,None).unpack()
        def backend(method,params=None):return call(U,R,'org.fsvxr.TestBackend',method,params)
        def api(method,params=None):return call(module.NAME,module.PATH,module.NAME,method,params)
        def wait(test):
            end=time.monotonic()+8
            while time.monotonic()<end:
                try:
                    value=test()
                    if value:return value
                except GLib.Error:pass
                time.sleep(.03)
            raise AssertionError('Timed out waiting for fake UDisks state')
        def snapshot():return json.loads(api('Snapshot')[0])
        def calls():return json.loads(backend('Calls')[0])
        try:
            wait(lambda:backend('Calls'))
            service=subprocess.Popen([sys.executable,str(HERE/'fsvr-devices.py')])
            wait(lambda:snapshot()['devices'] and snapshot()['devices'][0]['mounts'])
            assert calls()[0][0]=='Mount' and calls()[0][2]['auth.no_user_interaction'] is True
            if os.environ.get('FSVR_DEVICE_UI_TEST_BINARY'):
                subprocess.run(['xvfb-run','-a',os.environ['FSVR_DEVICE_UI_TEST_BINARY']],check=True,timeout=45)
            mounts_before=len([c for c in calls() if c[0]=='Mount'])
            api('Action',GLib.Variant('(sss)',('unmount',B,'')))
            wait(lambda:not snapshot()['devices'][0]['mounts'])
            time.sleep(.2);assert len([c for c in calls() if c[0]=='Mount'])==mounts_before,'Manual unmount remounted'
            result=json.loads(api('Action',GLib.Variant('(sss)',('mount',B,'')))[0])
            assert result['path']==temp+'/mounted'
            backend('Busy',GLib.Variant('(b)',(True,)))
            try:api('Action',GLib.Variant('(sss)',('remove',B,'')));raise AssertionError('Busy removal succeeded')
            except GLib.Error:pass
            assert not any(c[0]=='PowerOff' for c in calls()),'Powered off after unmount failure'
            backend('Busy',GLib.Variant('(b)',(False,)))
            api('SetAutomount',GLib.Variant('(b)',(False,)))
            assert json.loads(Path(temp+'/fsvr/automount.json').read_text())=={'enabled':False}
            assert (Path(temp+'/fsvr/automount.json').stat().st_mode&0o777)==0o600
            second=R+'/block_devices/sdz2'
            backend('Add',GLib.Variant('(s)',(second,)))
            wait(lambda:len(snapshot()['devices'])==2)
            assert not any(c[0]=='Mount' and c[1]==second for c in calls())
            api('SetAutomount',GLib.Variant('(b)',(True,)))
            wait(lambda:any(c[0]=='Mount' and c[1]==second for c in calls()))
            backend('AddEncrypted')
            encrypted=R+'/block_devices/luks'
            wait(lambda:any(r['id']==encrypted and r['locked'] for r in snapshot()['devices']))
            api('Action',GLib.Variant('(sss)',('unlock',encrypted,'fixture-passphrase')))
            wait(lambda:any(r['backing']==encrypted and r['mounts'] for r in snapshot()['devices']))
            api('Action',GLib.Variant('(sss)',('remove',B,'')))
            wait(lambda:not snapshot()['devices'])
            assert any(c[0]=='Lock' for c in calls()),'Removal did not lock encrypted container'
            assert 'fixture-passphrase' not in json.dumps(calls())
            assert calls()[-1][0]=='PowerOff'
            print('D-Bus integration: automount, explicit mount/open path, no remount, busy rejection, persistent toggle, hotplug, safe removal passed')
        finally:
            if service:service.terminate();service.wait(timeout=5)
            fake_process.terminate();fake_process.wait(timeout=5)

if __name__=='__main__':
    if '--fake' in sys.argv:fake()
    elif '--integration' in sys.argv:integration()
    else:unittest.main()
