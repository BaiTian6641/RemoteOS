#!/usr/bin/env python3
"""Portal contract fixture on a PRIVATE D-Bus and PRIVATE PipeWire server.

The fixture grants only generated test nodes. It does not request permissions
from, capture, or inject input into a real desktop. It is not a compositor
qualification test; the real PipeWire transport still runs unmodified.
"""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import gi
from gi.repository import Gio, GLib

PORTAL = '/org/freedesktop/portal/desktop'
REQUEST = 'org.freedesktop.portal.Request'
SESSION = 'org.freedesktop.portal.Session'
REMOTE = 'org.freedesktop.portal.RemoteDesktop'
SCREEN = 'org.freedesktop.portal.ScreenCast'
XML = '''<node>
<interface name="org.freedesktop.portal.RemoteDesktop">
 <property name="version" type="u" access="read"/>
 <property name="AvailableDeviceTypes" type="u" access="read"/>
 <method name="CreateSession"><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
 <method name="SelectDevices"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
 <method name="Start"><arg type="o" direction="in"/><arg type="s" direction="in"/><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
 <method name="NotifyKeyboardKeycode"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/><arg type="i" direction="in"/><arg type="u" direction="in"/></method>
 <method name="NotifyPointerButton"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/><arg type="i" direction="in"/><arg type="u" direction="in"/></method>
 <method name="NotifyPointerMotionAbsolute"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/><arg type="u" direction="in"/><arg type="d" direction="in"/><arg type="d" direction="in"/></method>
 <method name="NotifyPointerMotion"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/><arg type="d" direction="in"/><arg type="d" direction="in"/></method>
 <method name="NotifyPointerAxisDiscrete"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/><arg type="u" direction="in"/><arg type="i" direction="in"/></method>
</interface>
<interface name="org.freedesktop.portal.ScreenCast">
 <property name="version" type="u" access="read"/>
 <property name="AvailableSourceTypes" type="u" access="read"/>
 <property name="AvailableCursorModes" type="u" access="read"/>
 <method name="SelectSources"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
 <method name="OpenPipeWireRemote"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/><arg type="h" direction="out"/></method>
</interface>
<interface name="org.freedesktop.portal.Request"><method name="Close"/><signal name="Response"><arg type="u"/><arg type="a{sv}"/></signal></interface>
<interface name="org.freedesktop.portal.Session"><method name="Close"/><signal name="Closed"><arg type="a{sv}"/></signal></interface>
<interface name="org.remoteos.PortalTest">
 <method name="State"><arg type="s" direction="out"/></method>
 <method name="Mode"><arg type="s" direction="in"/></method>
 <method name="Revoke"/>
</interface>
</node>'''


class Fixture:
    def __init__(self, nodes, ready):
        self.nodes, self.ready = nodes, ready
        self.info = Gio.DBusNodeInfo.new_for_xml(XML)
        self.mode = 'normal'
        self.sessions = {}
        self.requests = {}
        self.state = {'created': 0, 'session_closed': 0, 'request_closed': 0,
                      'keys': [], 'buttons': [], 'absolute': [], 'relative': [], 'axes': [], 'key_calls': [], 'opened': 0}

    def acquired(self, bus, name):
        self.bus = bus
        for interface in (REMOTE, SCREEN, 'org.remoteos.PortalTest'):
            self.register(PORTAL, interface)
        GLib.timeout_add(100, self.link_generated_nodes)

    def link_generated_nodes(self):
        # Real desktop PipeWire policy normally makes these links. This test
        # has no device/session manager: link ONLY its named test sources.
        try:
            objects = json.loads(subprocess.check_output(['pw-dump'], text=True, timeout=2))
            ports = [v for v in objects if v['type'] == 'PipeWire:Interface:Port']
            links = {(v.get('info', {}).get('output-port-id'), v.get('info', {}).get('input-port-id'))
                     for v in objects if v['type'] == 'PipeWire:Interface:Link'}
            for obj in objects:
                props = obj.get('info', {}).get('props', {})
                if props.get('media.class') != 'Stream/Input/Video': continue
                target = str(props.get('target.object', ''))
                if target:
                    node = next((n for n in self.nodes if target == str(n['serial'])), None)
                else:
                    node = next((n for n in self.nodes if str(props.get('node.target', '')) == str(n['id'])), None)
                if not node: continue
                outport = next((p for p in ports if p['info']['props'].get('node.id') == node['id'] and p['info']['direction'] == 'output'), None)
                inport = next((p for p in ports if p['info']['props'].get('node.id') == obj['id'] and p['info']['direction'] == 'input'), None)
                if not outport or not inport or (outport['id'], inport['id']) in links: continue
                subprocess.run(['pw-link', '-L', str(outport['id']), str(inport['id'])], capture_output=True, text=True, check=True, timeout=2)
        except Exception as error: print('Private link policy:', error, flush=True)
        return GLib.SOURCE_CONTINUE

    def named(self, bus, name):
        self.ready.write_text('ready\n')

    def register(self, path, interface):
        return self.bus.register_object(path, self.info.lookup_interface(interface), self.method, self.property, None)

    def property(self, bus, sender, path, iface, name):
        values = {'version': 6 if iface == SCREEN else 2, 'AvailableDeviceTypes': 1 if self.mode == 'unsupported' else 3,
                  # Advertising all types must never cause the client to ask
                  # for MONITOR|VIRTUAL: KDE treats that as physical capture.
                  'AvailableSourceTypes': 1 if self.mode == 'no-virtual' else 7,
                  'AvailableCursorModes': 1 if self.mode == 'cursorless' else 3}
        return GLib.Variant('u', values[name])

    def reply(self, sender, options, invocation, results=None, response=0, pending=False):
        path = PORTAL + '/request/' + sender[1:].replace('.', '_') + '/' + options['handle_token']
        self.requests[path] = self.register(path, REQUEST)
        # Emit before returning the method result to catch subscription races.
        if not pending:
            self.bus.emit_signal(sender, path, REQUEST, 'Response', GLib.Variant('(ua{sv})', (response, results or {})))
        invocation.return_value(GLib.Variant('(o)', (path,)))

    def method(self, bus, sender, path, iface, method, parameters, invocation):
        p = parameters.unpack()
        if method not in ('State',): print(method, flush=True)
        try:
            if iface == 'org.remoteos.PortalTest':
                if method == 'State': invocation.return_value(GLib.Variant('(s)', (json.dumps(self.state),))); return
                if method == 'Mode':
                    self.mode = p[0]
                    for key in ('keys', 'buttons', 'absolute', 'relative', 'axes', 'key_calls'): self.state[key].clear()
                if method == 'Revoke':
                    for session, data in list(self.sessions.items()):
                        if data['sender'] == sender: continue
                        self.bus.emit_signal(data['sender'], session, SESSION, 'Closed', GLib.Variant('(a{sv})', ({},)))
                    self.state['keys'].clear(); self.state['buttons'].clear()
                invocation.return_value(None); return
            if iface == REQUEST and method == 'Close':
                self.state['request_closed'] += 1; invocation.return_value(None); return
            if iface == SESSION and method == 'Close':
                self.state['session_closed'] += 1
                self.sessions.pop(path, None); self.state['keys'].clear(); self.state['buttons'].clear()
                invocation.return_value(None); return
            if method == 'CreateSession':
                session = PORTAL + '/session/' + sender[1:].replace('.', '_') + '/' + p[0]['session_handle_token']
                self.sessions[session] = {'sender': sender, 'id': self.register(session, SESSION)}
                self.state['created'] += 1
                self.reply(sender, p[0], invocation, {'session_handle': GLib.Variant('s', session)}); return
            if p[0] not in self.sessions or self.sessions[p[0]]['sender'] != sender:
                raise ValueError('Session belongs to a different sender')
            if method == 'SelectDevices':
                assert p[1]['types'] == 3 and 'persist_mode' not in p[1]
                self.reply(sender, p[1], invocation); return
            if method == 'SelectSources':
                virtual = self.mode in ('virtual-only', 'virtual-fixed', 'virtual-physical', 'virtual-count')
                assert p[1]['types'] == (4 if virtual else 1) and p[1]['multiple'] == (not virtual) and p[1]['cursor_mode'] == 2
                assert 'persist_mode' not in p[1] and 'restore_token' not in p[1]
                self.reply(sender, p[1], invocation); return
            if method == 'Start':
                streams = []
                for i, node in enumerate(self.nodes):
                    if self.mode in ('virtual-only', 'virtual-fixed', 'virtual-physical') and i: break
                    virtual = self.mode in ('virtual-match', 'virtual-only', 'virtual-fixed', 'virtual-count')
                    size = ((640, 480) if i == 0 else (800, 600)) if virtual else ((320, 240) if i == 0 else (400, 300))
                    if self.mode == 'virtual-fixed': size = (1920, 1080)
                    props = {'size': GLib.Variant('(ii)', (320 if i == 0 else 400, 240 if i == 0 else 300)),
                             'position': GLib.Variant('(ii)', (-320 if i == 0 else 0, 0)),
                             'source_type': GLib.Variant('u', 4 if virtual else 1)}
                    props['size'] = GLib.Variant('(ii)', size)
                    # Exercise modern serial targeting and older node-ID targeting.
                    if i == 0: props['pipewire-serial'] = GLib.Variant('t', int(node['serial']))
                    streams.append((node['id'], props))
                if self.mode.startswith('kde-native'):
                    # This mode runs with a REAL private software KWin. Add its
                    # newly created native output alongside decoy monitor nodes.
                    objects = json.loads(subprocess.check_output(['pw-dump'], text=True, timeout=5))
                    native = next(v for v in objects if any(v.get('info', {}).get('props', {}).get(key, '').replace('kwin-screencast-Virtual-', 'kwin-screencast-').startswith('kwin-screencast-RemoteOS-HP2140-') for key in ('node.name', 'media.name')))
                    width, height = (1366, 768) if '1366' in self.mode else (1024, 576)
                    if 'wrong-size' in self.mode: width += 2
                    if 'foreign' not in self.mode:
                        streams.append((native['id'], {'size': GLib.Variant('(ii)', (width, height)),
                            'source_type': GLib.Variant('u', 1),
                            'pipewire-serial': GLib.Variant('t', int(native['info']['props']['object.serial']))}))
                if self.mode == 'too-many': streams.append((999, {}))
                if self.mode == 'bad-size': streams[0][1]['size'] = GLib.Variant('(ii)', (-1, 240))
                granted = {'devices': GLib.Variant('u', 2 if self.mode == 'denied' else 3),
                           'streams': GLib.Variant('a(ua{sv})', streams)}
                self.reply(sender, p[2], invocation, granted, response=1 if self.mode == 'cancel' else 0, pending=self.mode == 'pending'); return
            if method == 'OpenPipeWireRemote':
                connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                connection.connect(str(Path(os.environ['XDG_RUNTIME_DIR']) / 'pipewire-0'))
                fds = Gio.UnixFDList.new(); index = fds.append(connection.fileno())
                invocation.return_value_with_unix_fd_list(GLib.Variant('(h)', (index,)), fds)
                connection.close(); self.state['opened'] += 1; return
            if method.startswith('Notify'):
                if method in ('NotifyKeyboardKeycode', 'NotifyPointerButton'):
                    collection = 'keys' if method == 'NotifyKeyboardKeycode' else 'buttons'
                    code, down = p[2:]
                    if down and code not in self.state[collection]: self.state[collection].append(code)
                    if not down and code in self.state[collection]: self.state[collection].remove(code)
                    if collection == 'keys': self.state['key_calls'].append([code, down])
                elif method == 'NotifyPointerMotionAbsolute': self.state['absolute'].append(list(p[2:]))
                elif method == 'NotifyPointerMotion': self.state['relative'].append(list(p[2:]))
                elif method == 'NotifyPointerAxisDiscrete': self.state['axes'].append(list(p[2:]))
                if self.mode == 'slow-ack':
                    # Apply input immediately, but emulate a portal that takes
                    # 100 ms to acknowledge each call. The host must not wait
                    # for that reply while holding its capture/input mutex.
                    GLib.timeout_add(100, lambda: (invocation.return_value(None), GLib.SOURCE_REMOVE)[1])
                    return
                invocation.return_value(None); return
            raise ValueError('Unexpected portal call: ' + method)
        except Exception as error:
            invocation.return_dbus_error('org.remoteos.PortalTest.Error', str(error))


def main():
    if os.environ.get('REMOTEOS_PRIVATE_PORTAL_TEST') != '1': raise SystemExit('Private fixture only')
    fixture = Fixture(json.loads(Path(sys.argv[1]).read_text()), Path(sys.argv[2]))
    owner = Gio.bus_own_name(Gio.BusType.SESSION, 'org.freedesktop.portal.Desktop', Gio.BusNameOwnerFlags.NONE,
                             fixture.acquired, fixture.named, None)
    GLib.MainLoop().run()


if __name__ == '__main__': main()
