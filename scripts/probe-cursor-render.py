#!/usr/bin/env python3
"""Check cursor pixels rendered by the client on an isolated, authorized Xvfb.

Xvfb's -fbdir XWD includes the cursor. Comparing it before/after XFixesHideCursor
tests drawn pixels, rather than just the cursor image returned by XFixes.
This does not qualify the HP's i915/SNA framebuffer or hardware cursor plane.
"""
import argparse
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import secrets
import struct
import subprocess
import time


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def framebuffer(path):
    data = path.read_bytes()
    header = struct.unpack('>25I', data[:100])
    size, width, height, bpp, stride, colors = (header[i] for i in (0, 4, 5, 11, 12, 19))
    if header[1] != 7 or bpp != 32 or header[7] != 0:
        raise RuntimeError('Require XWD version 7 with 32-bit little-endian pixels')
    start = size + colors * 12
    pixels = data[start:start + stride * height]
    if len(pixels) != stride * height:
        raise RuntimeError('Incomplete Xvfb framebuffer')
    return width, height, stride, pixels


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--xvfb', type=Path, required=True)
    parser.add_argument('--client', type=Path, required=True, help='Client binary whose hash is recorded')
    parser.add_argument('--cursor-tool', type=Path, required=True, help='Cursor binary whose hash is recorded')
    parser.add_argument('--client-runner', type=Path, help='Optional target-library launcher')
    parser.add_argument('--cursor-runner', type=Path, help='Optional target-library launcher')
    parser.add_argument('--icons', type=Path, required=True)
    parser.add_argument('--assets', type=Path, required=True, help='Packaged Carbon CSS directory')
    parser.add_argument('--output', type=Path, required=True, help='New private evidence directory')
    parser.add_argument('--display', default=':97')
    args = parser.parse_args()
    if not args.display.startswith(':') or not args.display[1:].isdigit():
        parser.error('Require a local X display number')
    args.output.mkdir(parents=True, mode=0o700, exist_ok=False)
    out = args.output.resolve()
    for name in ('runtime', 'state', 'framebuffer'):
        (out / name).mkdir(mode=0o700)
    authority = out / 'Xauthority'
    authority.touch(mode=0o600)
    subprocess.run(['xauth', '-f', str(authority), 'add', args.display,
                    'MIT-MAGIC-COOKIE-1', secrets.token_hex(16)], check=True, timeout=5)
    env = dict(os.environ, DISPLAY=args.display, XAUTHORITY=str(authority),
               XDG_RUNTIME_DIR=str(out / 'runtime'), XCURSOR_THEME='Adwaita',
               XCURSOR_SIZE='24', XCURSOR_PATH=str(args.icons.resolve()),
               REMOTEOS_ASSET_DIR=str(args.assets.resolve()))
    x = c.CDLL('libX11.so.6')
    xt = c.CDLL('libXtst.so.6')
    xf = c.CDLL('libXfixes.so.3')
    x.XOpenDisplay.argtypes = [c.c_char_p]
    x.XOpenDisplay.restype = c.c_void_p
    x.XDefaultRootWindow.argtypes = [c.c_void_p]
    x.XDefaultRootWindow.restype = c.c_ulong
    x.XCloseDisplay.argtypes = [c.c_void_p]
    x.XSync.argtypes = [c.c_void_p, c.c_int]
    xf.XFixesHideCursor.argtypes = [c.c_void_p, c.c_ulong]
    xf.XFixesShowCursor.argtypes = [c.c_void_p, c.c_ulong]
    xt.XTestFakeMotionEvent.argtypes = [c.c_void_p, c.c_int, c.c_int, c.c_int, c.c_ulong]
    # Only this private server receives synthetic motion; the client's evdev
    # broker and automatic host connections are explicitly disabled below.
    os.environ['XAUTHORITY'] = str(authority)
    display, app, hidden = None, None, False
    with (out / 'run.log').open('w') as log:
        xvfb = subprocess.Popen([str(args.xvfb.resolve()), args.display, '-screen', '0',
                                 '1024x576x24', '-nolisten', 'tcp', '-auth', str(authority),
                                 '-fbdir', str(out / 'framebuffer')], env=env, stdout=log, stderr=log)
        try:
            for _ in range(50):
                if xvfb.poll() is not None:
                    raise RuntimeError('Private Xvfb exited; inspect run.log')
                display = x.XOpenDisplay(args.display.encode())
                if display:
                    break
                time.sleep(0.1)
            if not display:
                raise RuntimeError('Private Xvfb connection timed out')
            tool = str((args.cursor_runner or args.cursor_tool).resolve())
            initialized = subprocess.check_output([tool, '--initialize'], env=env, text=True, timeout=5)
            app_command = [str((args.client_runner or args.client).resolve()), '--no-input',
                           '--no-autoconnect', '--no-discovery', '--state-dir', str(out / 'state')]
            app = subprocess.Popen(app_command, env=env, stdout=log, stderr=log)
            time.sleep(2)
            if app.poll() is not None:
                raise RuntimeError('Client exited; inspect run.log')
            root = x.XDefaultRootWindow(display)
            observations = []
            for name, xpos, ypos in [('welcome body', 300, 200), ('header', 250, 30)]:
                xt.XTestFakeMotionEvent(display, -1, xpos, ypos, 0)
                x.XSync(display, False)
                time.sleep(0.15)
                response = subprocess.check_output([tool], env=env, text=True, timeout=5)
                image = json.loads(response.removeprefix('X11 cursor: '))
                width, height, stride, visible = framebuffer(out / 'framebuffer/Xvfb_screen0')
                if not observations:
                    # Preserve a viewable screen containing the actual sprite.
                    # PPM avoids adding an image-library dependency to the probe.
                    rgb = bytearray()
                    for y in range(height):
                        row = visible[y * stride:y * stride + width * 4]
                        for pixel in range(width):
                            start = pixel * 4
                            rgb.extend((row[start + 2], row[start + 1], row[start]))
                    (out / 'cursor-visible.ppm').write_bytes(
                        f'P6\n{width} {height}\n255\n'.encode() + rgb)
                xf.XFixesHideCursor(display, root)
                hidden = True
                x.XSync(display, False)
                _, _, _, absent = framebuffer(out / 'framebuffer/Xvfb_screen0')
                xf.XFixesShowCursor(display, root)
                hidden = False
                x.XSync(display, False)
                left = image['position'][0] - image['hotspot'][0]
                top = image['position'][1] - image['hotspot'][1]
                changed = 0
                for y in range(max(0, top), min(height, top + image['size'][1])):
                    for px in range(max(0, left), min(width, left + image['size'][0])):
                        start = y * stride + px * 4
                        changed += visible[start:start+4] != absent[start:start+4]
                row = dict(location=name, cursor=image, rendered_changed_pixels=changed)
                if not (image['name'] == 'left_ptr' and image['argb_supported'] and
                        image['size'] == [24, 24] and image['light_pixels'] > 0 and
                        image['dark_pixels'] > 0 and changed >= 50):
                    raise RuntimeError('Cursor render check failed: ' + json.dumps(row))
                observations.append(row)
            receipt = dict(status='passed', client=str(args.client.resolve()),
                           client_sha256=digest(args.client), cursor_tool=str(args.cursor_tool.resolve()),
                           cursor_tool_sha256=digest(args.cursor_tool),
                           carbon_css_sha256=digest(args.assets / 'carbon.css'),
                           initialized=json.loads(initialized.removeprefix('X11 cursor: ')),
                           isolated_server=args.display, physical_input_enabled=False,
                           auto_connect_enabled=False, discovery_enabled=False, observations=observations,
                           hardware_qualified=False,
                           limits='Verifies actual cursor pixels in Xvfb; does not verify Intel i915/SNA rendering on the HP.')
            (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
            print(json.dumps(receipt, indent=2))
        finally:
            if display:
                if hidden:
                    xf.XFixesShowCursor(display, x.XDefaultRootWindow(display))
                    x.XSync(display, False)
                x.XCloseDisplay(display)
            for process in (app, xvfb):
                if process and process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()


if __name__ == '__main__':
    main()
