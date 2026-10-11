#!/usr/bin/env python3
"""Real TLS sockets: pairing, saved pins, wrong host identity, unpaired denial."""
import json, pathlib, subprocess, sys, tempfile, socket, ssl, struct
host,ctl,probe=sys.argv[1:]
def run(args,ok=True):
    p=subprocess.run(args,text=True,capture_output=True,timeout=15)
    if (p.returncode==0)!=ok: raise AssertionError(f'{args[0]} exit {p.returncode}: {p.stdout} {p.stderr}')
    return p.stdout
with tempfile.TemporaryDirectory(prefix='remoteos-integration-') as tmp:
    root=pathlib.Path(tmp)
    info=json.loads(run([host,'--identity',str(root/'host'),'--init']))
    server=subprocess.Popen([host,'--identity',str(root/'host'),'--bind','127.0.0.1','--port','0','--pair-code-file',info['pairing_code_file']],text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    try:
        started=json.loads(server.stdout.readline());port=str(started['port'])
        base=[ctl,'--host','127.0.0.1','--port',port,'--identity',str(root/'client'),'--fingerprint',info['fingerprint']]
        run(base,ok=False)
        paired=run(base+['--pair-code-file',info['pairing_code_file']])
        assert '"type":"paired"' in paired and '"type":"capabilities"' in paired
        assert not pathlib.Path(info['pairing_code_file']).exists()
        run([ctl,'--host','127.0.0.1','--port',port,'--identity',str(root/'client')])
        run(base[:-1]+['0'*64],ok=False)
        run([ctl,'--host','127.0.0.1','--port',port,'--identity',str(root/'stranger'),'--fingerprint',info['fingerprint']],ok=False)
        # Oversized frame rejected before allocation; authenticate TLS with paired cert.
        run([probe,'127.0.0.1',port,str(root/'client')])
        # Even a paired TLS identity must authenticate the application session;
        # unauthorized traffic cannot keep the sole control slot occupied.
        run([probe,'127.0.0.1',port,str(root/'stranger'),'unauthorized-ping'])
        run([ctl,'--host','127.0.0.1','--port',port,'--identity',str(root/'client')])
        print('TLS pairing, persisted identity, rejection and frame-limit integration passed')
    finally:
        server.terminate()
        try:server.wait(timeout=5)
        except subprocess.TimeoutExpired:server.kill();server.wait()
    # Pairing code is consumed; its configured pathname may remain across boot.
    server=subprocess.Popen([host,'--identity',str(root/'host'),'--port','0','--pair-code-file',info['pairing_code_file']],text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    try:
        started=json.loads(server.stdout.readline())
        run([ctl,'--host','127.0.0.1','--port',str(started['port']),'--identity',str(root/'client'),'--fingerprint',info['fingerprint']])
    finally:
        server.terminate()
        try:server.wait(timeout=5)
        except subprocess.TimeoutExpired:server.kill();server.wait()
