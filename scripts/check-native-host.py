#!/usr/bin/env python3
"""Portable host process + pinned TLS pairing + generated MPEG-2/UDP only.

No screen capture, physical input, privacy grants, camera or SD card access.
"""
import hashlib,json,pathlib,queue,socket,ssl,struct,subprocess,sys,tempfile,threading,time
host=str(pathlib.Path(sys.argv[1]).resolve())
def start(config,lines):
    process=subprocess.Popen([host,'--desktop','--config',str(config)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,encoding='utf-8',errors='replace')
    def read():
        for line in process.stdout: lines.put(line.rstrip())
    threading.Thread(target=read,daemon=True).start()
    return process
with tempfile.TemporaryDirectory(prefix='remoteos-native-') as temp:
    root=pathlib.Path(temp)/'Portable host 测试';root.mkdir()
    def init(name):
        r=subprocess.run([host,'--identity',str(root/name),'--init'],capture_output=True,text=True,encoding='utf-8',timeout=15)
        if r.returncode: raise RuntimeError('Identity initialization failed: '+r.stdout+r.stderr)
        return json.loads(r.stdout)
    identity=init('host');client=init('client')
    config=root/'host.json';base={'application_mode':True,'identity_dir':str(root/'host'),'pairing_code_file':identity['pairing_code_file'],'bind':'127.0.0.1','port':0,'resource_port':0,'userspace_devices':True,'usb_enabled':False,'audio_source':'none','media_transport':'rtp-udp','discovery_enabled':False,'test_source':True}
    config.write_text(json.dumps(base),encoding='utf-8');lines=queue.Queue();process=start(config,lines);events=[]
    try:
        deadline=time.monotonic()+15;listening=None
        while time.monotonic()<deadline:
            try: line=lines.get(timeout=.2)
            except queue.Empty:
                if process.poll() is not None:break
                continue
            events.append(line)
            try: event=json.loads(line)
            except ValueError:continue
            if event.get('event')=='sharing-failed':raise RuntimeError(event['message'])
            if event.get('event')=='listening':listening=event;break
        if not listening:raise RuntimeError('Host did not listen: '+'; '.join(events[-12:]))
        ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT);ctx.check_hostname=False;ctx.verify_mode=ssl.CERT_NONE;ctx.minimum_version=ssl.TLSVersion.TLSv1_3
        ctx.load_cert_chain(str(root/'client/certificate.pem'),str(root/'client/private-key.pem'))
        with ctx.wrap_socket(socket.create_connection(('127.0.0.1',listening['port']),timeout=5),server_hostname='RemoteOS test') as control:
            assert hashlib.sha256(control.getpeercert(binary_form=True)).hexdigest()==identity['fingerprint'],'Pinned identity changed'
            def send(message):
                message['version']=1;data=json.dumps(message).encode();control.sendall(struct.pack('!I',len(data))+data)
            def exact(count):
                data=b''
                while len(data)<count:
                    chunk=control.recv(count-len(data))
                    if not chunk:raise RuntimeError('Control closed unexpectedly')
                    data+=chunk
                return data
            def receive(kind):
                for _ in range(30):
                    size=struct.unpack('!I',exact(4))[0];assert 0<size<1048576
                    event=json.loads(exact(size))
                    if event.get('type')=='error':raise RuntimeError(str(event))
                    if event.get('type')==kind:return event
                raise RuntimeError('Missing control response: '+kind)
            send({'type':'pair','code':pathlib.Path(identity['pairing_code_file']).read_text().strip()});receive('paired')
            send({'type':'hello'});caps=receive('capabilities');assert caps['test_source'] and caps['host_mode']=='application' and not caps['input']
            with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as udp:
                udp.bind(('127.0.0.1',48020));udp.settimeout(3)
                display={'id':'internal','width':320,'height':240,'encoded_width':320,'encoded_height':240,'x':0,'y':0,'fps':30,'bitrate':2000000,'codec':'mpeg2'}
                send({'type':'start','displays':[display],'media_transports':['rtp-udp'],'mpeg2_payloads':['gst-frame'],'low_latency':True,'bandwidth_limit_bps':40000000,'match_host_resolution':False,'client_fingerprint':client['fingerprint']})
                offer=receive('offer');assert offer['media_transport']=='rtp-udp'
                send({'type':'media_ready'});timestamps=set();packets=0;end=time.monotonic()+5
                while len(timestamps)<3 and time.monotonic()<end:
                    packet,_=udp.recvfrom(2048)
                    assert len(packet)>=12 and packet[0]>>6==2
                    assert struct.unpack('!I',packet[8:12])[0]==offer['streams'][0]['ssrc']
                    timestamps.add(packet[4:8]);packets+=1
                assert len(timestamps)>=3,'Generated MPEG-2 stopped after startup'
            send({'type':'disconnect'})
            print(json.dumps({'status':'passed','pinned_tls':control.version(),'unicode_identity_path':True,'application_host_listener':True,'generated_mpeg2_frames':len(timestamps),'rtp_packets':packets,'physical_devices_used':False,'permissions_requested':False}))
    finally:
        if process.poll() is None:process.terminate()
        try:process.wait(timeout=5)
        except subprocess.TimeoutExpired:process.kill();process.wait(timeout=3)
    if sys.platform in ('win32','darwin'):
        # Probe real desktop permission gates without starting capture or injecting
        # input. CI must surface a missing grant as a structured failure, not crash.
        base['test_source']=False;config.write_text(json.dumps(base),encoding='utf-8');lines=queue.Queue();process=start(config,lines)
        try:
            deadline=time.monotonic()+15;event=None;output=[]
            while time.monotonic()<deadline:
                try:line=lines.get(timeout=.2)
                except queue.Empty:
                    if process.poll() is not None:break
                    continue
                output.append(line)
                try:x=json.loads(line)
                except ValueError:continue
                if x.get('event') in ('listening','sharing-failed'):event=x;break
            if not event:raise RuntimeError('Permission gate exited without diagnostics: '+'; '.join(output[-12:]))
            if event['event']=='sharing-failed':
                expected=('Screen Recording','Accessibility','ScreenCaptureKit','display grant') if sys.platform=='darwin' else ('logged-in desktop','user session is disconnected','Desktop access denied','interactive Windows session')
                assert any(text in event['message'] for text in expected),event
                assert process.wait(timeout=5)==1,'Denied permission did not produce a controlled exit'
            print(json.dumps({'status':'passed','permission_gate':event['event'],'message':event.get('message','Desktop metadata available; capture not started'),'permissions_requested':False,'physical_capture_started':False}))
        finally:
            if process.poll() is None:process.terminate()
            try:process.wait(timeout=5)
            except subprocess.TimeoutExpired:process.kill();process.wait(timeout=3)
