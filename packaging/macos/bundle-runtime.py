#!/usr/bin/env python3
"""Stage the official universal GStreamer runtime and relocate Mach-O links."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import json

MAGIC={b'\xfe\xed\xfa\xce',b'\xce\xfa\xed\xfe',b'\xfe\xed\xfa\xcf',b'\xcf\xfa\xed\xfe',b'\xca\xfe\xba\xbe',b'\xbe\xba\xfe\xca',b'\xca\xfe\xba\xbf',b'\xbf\xba\xfe\xca'}
def macho(path):
    if path.is_symlink() or not path.is_file():return False
    with path.open('rb') as stream:return stream.read(4) in MAGIC
def run(*args):return subprocess.check_output(args,text=True).strip()
def dependencies(path):return [line.strip().split(' (')[0] for line in run('otool','-L',str(path)).splitlines()[1:] if '(compatibility version ' in line]
def rpaths(path):return re.findall(r'cmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset',run('otool','-l',str(path)))
def system(path):return path.startswith(('/usr/lib/','/System/Library/'))

def bundle(app,sdk):
    sdk=sdk.resolve();framework=app/'Contents/Frameworks/GStreamer.framework'
    root=framework/'Versions/1.0';root.mkdir(parents=True)
    ignore=shutil.ignore_patterns('*.a','*.la','*.pc','pkgconfig','cmake','girepository-1.0','include')
    for directory in ('lib','libexec','etc','share/licenses','Resources'):
        src=sdk/directory
        if src.exists():shutil.copytree(src,root/directory,symlinks=True,ignore=ignore)
    if (sdk/'GStreamer').exists():
        shutil.copy2(sdk/'GStreamer',root/'GStreamer')
        (framework/'GStreamer').symlink_to('Versions/Current/GStreamer')
    (root/'bin').mkdir()
    for name in ('gst-inspect-1.0','gst-launch-1.0'):shutil.copy2(sdk/'bin'/name,root/'bin'/name)
    (framework/'Versions/Current').symlink_to('1.0')
    for name,target in [('Resources','Resources'),('Libraries','lib')]:
        (framework/name).symlink_to('Versions/Current/'+target)
    if not (root/'share/licenses').is_dir():raise RuntimeError('SDK licenses are missing')
    # Resolve links from each loader, including the nested plugin scanner.
    for path in [path for path in app.rglob('*') if macho(path)]:
        os.chmod(path,path.stat().st_mode|0o200)
        ids=[line.strip() for line in run('otool','-D',str(path)).splitlines()[1:] if line.startswith(('/', '@')) and not line.endswith(':')]
        if ids:subprocess.run(['install_name_tool','-id','@loader_path/'+path.name,str(path)],check=True)
        for dep in dependencies(path):
            if system(dep):continue
            target=None
            if '/GStreamer.framework/Versions/' in dep:
                suffix=dep.split('/GStreamer.framework/Versions/',1)[1].split('/',1)[1];target=root/suffix
            elif dep.startswith('@rpath/'):
                suffix=dep[len('@rpath/'):];candidates=[root/'lib'/suffix,root/suffix]
                if '/Versions/' in suffix:candidates.insert(0,framework.parent/suffix)
                target=next((item for item in candidates if item.exists()),None)
            elif dep.startswith('@loader_path/'):target=(path.parent/dep[len('@loader_path/'):]).resolve()
            else:raise RuntimeError('Unbundled dependency: '+dep+' in '+str(path))
            if target is None or not target.exists() or not target.resolve().is_relative_to(app.resolve()):raise RuntimeError('Dependency missing or escapes app: '+dep)
            replacement='@loader_path/'+os.path.relpath(target,path.parent)
            if replacement!=dep:subprocess.run(['install_name_tool','-change',dep,replacement,str(path)],check=True)
        for old in rpaths(path):
            if not old.startswith('@'):subprocess.run(['install_name_tool','-delete_rpath',old,str(path)],check=True)
    verify(app)

def verify(app):
    binaries=[path for path in app.rglob('*') if macho(path)]
    for path in binaries:
        arches=set(run('lipo','-archs',str(path)).split())
        if not {'x86_64','arm64'} <= arches:raise RuntimeError('Not universal: '+str(path)+' '+str(arches))
        for dep in dependencies(path):
            if system(dep):continue
            if not dep.startswith('@loader_path/'):raise RuntimeError('Nonportable dependency: '+dep+' in '+str(path))
            resolved=(path.parent/dep[len('@loader_path/'):]).resolve()
            if not resolved.is_relative_to(app.resolve()) or not resolved.exists():raise RuntimeError('Missing bundled dependency: '+dep)
        for old in rpaths(path):
            if old.startswith('/') and not system(old):raise RuntimeError('SDK rpath remains: '+old)
    print(json.dumps({'mach_o_files':len(binaries),'architectures':['x86_64','arm64'],'external_libraries':'macOS system only'}))

def main():
    p=argparse.ArgumentParser();p.add_argument('app',type=Path);p.add_argument('--sdk',type=Path);p.add_argument('--verify',action='store_true')
    a=p.parse_args();a.app=a.app.resolve()
    if a.verify:verify(a.app)
    else:
        if not a.sdk:p.error('--sdk is required')
        bundle(a.app,a.sdk)
if __name__=='__main__':main()
