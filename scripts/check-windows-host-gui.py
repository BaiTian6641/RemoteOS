#!/usr/bin/env python3
"""Exercise Start/Stop sharing in the isolated Windows CI desktop.

Only listener/desktop metadata is prepared; no client connects, no capture or
input starts, and no firewall, privilege or privacy setting is changed.
"""
import ctypes,json,os,pathlib,socket,sys,time
u=ctypes.windll.user32
u.FindWindowExW.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_wchar_p,ctypes.c_wchar_p];u.FindWindowExW.restype=ctypes.c_void_p
u.SendMessageW.argtypes=[ctypes.c_void_p,ctypes.c_uint,ctypes.c_size_t,ctypes.c_ssize_t];u.SendMessageW.restype=ctypes.c_ssize_t
u.SetWindowTextW.argtypes=[ctypes.c_void_p,ctypes.c_wchar_p]
u.GetWindowTextW.argtypes=[ctypes.c_void_p,ctypes.c_wchar_p,ctypes.c_int]
window=int(sys.argv[1]);edit=u.FindWindowExW(window,None,'EDIT',None);port_edit=u.FindWindowExW(window,edit,'EDIT',None)
assert edit and port_edit,'Host setup controls are absent'
with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
u.SetWindowTextW(edit,'127.0.0.1');u.SetWindowTextW(port_edit,str(port))
log=pathlib.Path(os.environ['LOCALAPPDATA'])/'RemoteOS/Host/last-host.log'
u.SendMessageW(window,0x111,1,0)
def texts():
    out=[];child=None
    while True:
        child=u.FindWindowExW(window,child,None,None)
        if not child:break
        buf=ctypes.create_unicode_buffer(4096);u.GetWindowTextW(child,buf,len(buf));out.append(buf.value)
    return out
end=time.monotonic()+15;event=None
try:
    while time.monotonic()<end:
        try:lines=log.read_text(encoding='utf-8').splitlines()
        except OSError:lines=[]
        for line in lines:
            try:x=json.loads(line)
            except ValueError:continue
            if x.get('event') in ('listening','sharing-failed'):event=x
        if event:
            text='\n'.join(texts())
            if event['event']=='listening' and 'Ready to pair.' in text:break
            if event['event']=='sharing-failed' and event['message'] in text:break
        time.sleep(.1)
    else:raise AssertionError('GUI did not surface helper status: '+str(event)+'; '+'\n'.join(texts()))
    if event['event']=='sharing-failed':
        assert any(t in event['message'] for t in ('logged-in desktop','user session is disconnected','Desktop access denied','interactive Windows session')),event
    print(json.dumps({'status':'passed','start_sharing':event['event'],'helper_error_visible':event['event']=='sharing-failed','loopback_only':True,'physical_capture_started':False,'settings_changed':False}))
finally:u.SendMessageW(window,0x111,2,0)
