#!/usr/bin/env python3
"""Stage the client from a private Ubuntu 24.04 rootfs; no host libs are copied."""
import argparse,hashlib,json,re,shutil,subprocess,os
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--root',type=Path,required=True);p.add_argument('--repo',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--build',type=Path);a=p.parse_args()
root=a.root.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
origins={};skip=re.compile(r'^(ld-linux|lib(c|m|pthread|dl|rt|resolv|util|nss_[a-z]+)\.so)')
def confined(src):
 path=Path(os.path.normpath(src));links=0
 while True:
  if not path.is_relative_to(root):raise RuntimeError('Dependency escaped baseline root')
  parts=path.relative_to(root).parts;current=root
  for i,part in enumerate(parts):
   current=current/part
   if current.is_symlink():
    target=os.readlink(current);base=root/target.lstrip('/') if target.startswith('/') else current.parent/target
    path=Path(os.path.normpath(base.joinpath(*parts[i+1:])));links+=1
    if links>40:raise RuntimeError('Dependency link loop')
    break
  else:return path

def copy(src,rel):
 src=confined(src);dst=out/rel
 if dst.exists():return
 if not src.is_relative_to(root):raise RuntimeError('Dependency escaped baseline root: '+str(src))
 dst.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(src,dst);origins[rel]=str(src.relative_to(root))
 dynamic=subprocess.check_output(['readelf','-d',str(src)],text=True)
 for needed in re.findall(r'\(NEEDED\).*\[(.*?)\]',dynamic):
  if skip.match(needed):continue
  found=next((b/needed for b in (root/'usr/lib/x86_64-linux-gnu',root/'lib/x86_64-linux-gnu',root/'usr/lib',root/'usr/lib/x86_64-linux-gnu/pulseaudio',root/'usr/lib/x86_64-linux-gnu/blas',root/'usr/lib/x86_64-linux-gnu/lapack') if (b/needed).exists()),None)
  if found is None:raise RuntimeError('Missing baseline dependency '+needed)
  copy(found,'usr/lib/'+needed)
copy((a.build or root/'build')/'remoteos-client','usr/bin/remoteos-client')
copy(root/'usr/lib/x86_64-linux-gnu/gstreamer1.0/gstreamer-1.0/gst-plugin-scanner','usr/libexec/gst-plugin-scanner')
plugins=['coreelements','app','udp','rtp','rtpmanager','dtls','srtp','libav','audioconvert','audioresample','opus','videoconvertscale','videoparsersbad','ximagesink','xvimagesink','pulseaudio','video4linux2','jpeg']
for name in plugins:copy(root/('usr/lib/x86_64-linux-gnu/gstreamer-1.0/libgst'+name+'.so'),'usr/lib/gstreamer-1.0/libgst'+name+'.so')
for src in (root/'usr/lib/x86_64-linux-gnu/gio/modules').glob('*.so'):
 if 'gnutls' in src.name:copy(src,'usr/lib/gio/modules/'+src.name)
for name in ('AppRun','remoteos-client.desktop','remoteos-client.svg'):shutil.copy2(a.repo/'packaging/appimage'/name,out/name)
shutil.copytree(a.repo/'assets',out/'usr/share/remoteos')
if (a.repo/'output/hp2140/target/usr/share/fonts/ibm-plex').is_dir():shutil.copytree(a.repo/'output/hp2140/target/usr/share/fonts/ibm-plex',out/'usr/share/fonts/ibm-plex')
shutil.copytree(root/'usr/share/glib-2.0/schemas',out/'usr/share/glib-2.0/schemas')
# Record every package version in the baseline, and retain distro copyright
# files (including license text/source locations) for the bundled dependencies.
for folder in (root/'usr/share/doc').iterdir():
 source=confined(folder/'copyright')
 if source.is_file():
  dest=out/'licenses/ubuntu24'/folder.name/'copyright';dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,dest)
shutil.copytree(root/'usr/share/common-licenses',out/'licenses/common-licenses')
shutil.copytree(a.repo/'third_party/cpp-httplib',out/'licenses/cpp-httplib',ignore=shutil.ignore_patterns('httplib.h'))
for directory,name in [('asio','LICENSE_1_0.txt'),('nlohmann','LICENSE.MIT')]:
 dest=out/'licenses'/directory;dest.mkdir(parents=True,exist_ok=True);shutil.copy2(a.repo/'third_party'/directory/name,dest/name)
shutil.copy2(a.repo/'third_party/NOTICE.md',out/'licenses/NOTICE.md')
shutil.copy2(a.repo/'VERSION',out/'VERSION')
for src in (a.repo/'output/hp2140/target/usr/share/licenses/ibm-plex/LICENSE.txt',a.repo/'packaging/appimage/runtime-lock.json'):
 if src.exists():shutil.copy2(src,out/'licenses'/src.name)
shutil.copy2(root/'var/lib/dpkg/status',out/'licenses/ubuntu-package-status.txt')
# Dependencies are supplied through a relocatable AppRun library path.
versions=[]
for rel in origins:
 text=subprocess.check_output(['readelf','--version-info',str(out/rel)],text=True)
 versions+=re.findall(r'GLIBC_(\d+\.\d+)',text)
maxv=max(versions,key=lambda v:tuple(map(int,v.split('.'))))
if tuple(map(int,maxv.split('.')))>(2,39):raise RuntimeError('Unexpected newer glibc ABI '+maxv)
manifest={'version':(a.repo/'VERSION').read_text().strip(),'baseline':'Ubuntu 24.04 x86_64','max_glibc':maxv,'input':'focused GTK; Ctrl+Alt+Home opens overlay','presentation':'X11 or XWayland, ximagesink; PulseAudio/PipeWire Pulse','files':[{'path':str(f.relative_to(out)),'sha256':hashlib.sha256(f.read_bytes()).hexdigest(),'source':origins.get(str(f.relative_to(out)),'asset/license')} for f in sorted(out.rglob('*')) if f.is_file()]}
(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n');print(json.dumps({'files':len(manifest['files']),'max_glibc':maxv}))
