#!/usr/bin/env python3
"""Extract Ubuntu development packages locally; never install/change host packages."""
import concurrent.futures, hashlib, os, pathlib, re, shutil, subprocess, sys, tempfile
root = pathlib.Path(__file__).resolve().parents[1]
prefix = root / '.deps'
cache = root / '.cache' / 'dev-debs'
cache.mkdir(parents=True, exist_ok=True)
packages = ['libgtk-3-dev','libgstreamer1.0-dev','libgstreamer-plugins-base1.0-dev',
 'libwebkit2gtk-4.1-dev','libdrm-dev','libx11-dev','libxtst-dev','libxrandr-dev','libxfixes-dev',
 'libwayland-dev','libpipewire-0.3-dev',
 'gstreamer1.0-tools','gstreamer1.0-plugins-bad','xvfb']
result = subprocess.run(['apt-get','--print-uris','--yes','--download-only','--no-install-recommends',
 '-o','Debug::NoLocking=1','install',*packages],check=True,text=True,capture_output=True)
entries = re.findall(r"^'([^']+)' (\S+) (\d+) MD5Sum:([0-9a-f]+)$",result.stdout,re.M)
def fetch(entry):
    url,name,size,digest=entry
    path=cache/name
    if not path.exists():
        subprocess.run(['curl','-fsSL','--retry','2','--max-time','120',url.replace('http://','https://'),'-o',str(path)],check=True)
    if len(path.read_bytes()) != int(size) or hashlib.md5(path.read_bytes()).hexdigest()!=digest:
        raise RuntimeError('Package verification failed: '+name)
    return path
with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
    paths=list(pool.map(fetch,entries))
for path in paths:
    subprocess.run(['dpkg-deb','--extract',str(path),str(prefix)],check=True)
for path in (prefix/'usr/lib').rglob('*'):
    if path.is_symlink() and not path.exists():
        native=pathlib.Path('/')/path.relative_to(prefix)
        if not native.exists():
            # A development .so symlink can refer to a runtime library that
            # apt already installed, so it is absent from this extraction.
            native=native.parent/pathlib.Path(os.readlink(path)).name
        if native.exists():
            path.unlink()
            path.symlink_to(native.resolve())
for path in prefix.rglob('*.pc'):
    contents=path.read_text()
    contents=re.sub(r'(?m)^prefix=/usr$', 'prefix='+str(prefix/'usr'),contents)
    contents=re.sub(r'(?m)^libdir=/usr/', 'libdir='+str(prefix/'usr')+'/',contents)
    contents=re.sub(r'(?m)^includedir=/usr/', 'includedir='+str(prefix/'usr')+'/',contents)
    path.write_text(contents)

def build_pinned_evdi():
    # The distro libevdi-dev version need not match the host ABI we qualify.
    # Reuse the release downloader and verify source on repeat invocations;
    # never replace a developer's changed checkout or install a kernel module.
    lock_path=root/'packaging/linux/dependencies.lock.json'
    import json
    lock=json.loads(lock_path.read_text())['evdi']
    source=prefix/lock['source_directory']
    archive_name=f'evdi-{lock["version"]}.tar.gz'
    archive=prefix/archive_name
    with tempfile.TemporaryDirectory(prefix='.evdi-verify-',dir=prefix) as temporary:
        stage=pathlib.Path(temporary)
        if archive.exists():
            shutil.copyfile(archive,stage/archive_name)
        subprocess.run([sys.executable,str(root/'packaging/linux/get-dependencies.py'),str(stage)],check=True)
        verified=stage/lock['source_directory']
        if source.exists():
            for clean in verified.rglob('*'):
                existing=source/clean.relative_to(verified)
                if clean.is_symlink():
                    same=existing.is_symlink() and os.readlink(existing)==os.readlink(clean)
                elif clean.is_dir():
                    same=existing.is_dir() and not existing.is_symlink()
                else:
                    same=existing.is_file() and not existing.is_symlink() and existing.read_bytes()==clean.read_bytes()
                if not same:
                    raise RuntimeError('Pinned EVDI source changed; preserve your edits and remove/rename '+str(source))
        else:
            verified.rename(source)
        if not archive.exists():
            shutil.copyfile(stage/archive_name,archive)
    environment=os.environ.copy()
    pc_paths=[str(prefix/'usr/lib/x86_64-linux-gnu/pkgconfig'),str(prefix/'usr/share/pkgconfig')]
    if environment.get('PKG_CONFIG_PATH'):
        pc_paths.append(environment['PKG_CONFIG_PATH'])
    environment['PKG_CONFIG_PATH']=':'.join(pc_paths)
    subprocess.run(['make','-B','-C',str(source/'library'),
                    'PKG_CONFIG='+environment.get('PKG_CONFIG','pkg-config')],env=environment,check=True)
    print('Built verified EVDI userspace library',source/'library'/f'libevdi.so.{lock["version"]}')

build_pinned_evdi()
print('Extracted',len(paths),'packages into',prefix)
print('Use: . cmake/dev-env.sh')
