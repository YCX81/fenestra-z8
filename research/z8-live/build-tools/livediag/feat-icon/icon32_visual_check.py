#!/usr/bin/env python3
"""Measure the pinned MAIN resources and archived 1024x682 GUI at 1:1.
The optional preview composites generated badges over *archived QEMU* output.
"""
import argparse
import hashlib
import json
import math
import shutil
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

import icon32_generate as art

HERE = Path(__file__).resolve().parent
ARCHIVE = HERE.parents[2] / '_mac-migration/files/_bundle-20261102/codex-tasks/live-playback-ui/evidence'
PINNED = {
    'bg34.main': 'c7c6dbeea8a94064a10d47a7b9022ab26e4aa6b12ba5fb63b1ad85abac4fe4d4',
    'before-playback-shooting.png': '2d42673b8809e4c3640ce04ef6565de3282599267dfd2f813fb29969b33bed3a',
    'pb-no-live.png': '30b51ce3f467809b82f305eb6c9124818a8df4edbb73057f8dcc56abd09ce1cf',
}
BASE = 0x40160000


def pinned(path):
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != PINNED[path.name]:
        raise ValueError(f'input identity differs: {path}')
    return data


def decode(binary, source, pixfmt):
    path = str(source) if isinstance(source, Path) else 'pipe:0'
    data = None if isinstance(source, Path) else source
    args = [binary, '-v', 'error', '-xerror', '-i', path, '-vsync', '0',
            '-f', 'rawvideo', '-pix_fmt', pixfmt, 'pipe:1']
    return subprocess.run(args, input=data, capture_output=True, check=True).stdout


def dimensions(png):
    if png[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('expected PNG')
    return struct.unpack_from('>II', png, 16)


def bright_bounds(data, width, box, channels):
    # Require every colour channel to be bright, excluding the coloured bars.
    left, top, right, bottom = box
    xs, ys = [], []
    for y in range(top, bottom):
        for x in range(left, right):
            offset = (y*width+x)*channels
            if min(data[offset:offset+channels]) >= 224:
                xs.append(x)
                ys.append(y)
    if not xs:
        raise ValueError(f'no bright ink in ROI {box}')
    return [min(xs), min(ys), max(xs)+1, max(ys)+1]


def stamp(rgb, canvas_w, x, y, rgba, w, h):
    # Composite straight RGBA. A zero-coverage pixel must retain the exact
    # archived colour. The perimeter and holes are transparent at every phase.
    for row in range(h):
        for col in range(w):
            src = 4*(row*w+col)
            alpha = rgba[src+3]
            dst = 3*((row+y)*canvas_w+x+col)
            for channel in range(3):
                rgb[dst+channel] = (rgba[src+channel]*alpha + rgb[dst+channel]*(255-alpha)+127)//255


def check_target_assets():
    # Synthetic device IDs exercise exact matching, odd-sized packing, colour
    # modes, the maximum storage bound and the zero-profile default fallback.
    source = """#include <stdio.h>
#include "shoot.h"
#include "pb.h"
int main(void) {
    const uint32_t targets[][3]={{7,1,4},{7,1,5},{8,1,4},{7,2,4},{7,1,6},{7,1,4}};
    _Static_assert(ICON32_SHOOT_MAX_PIXELS==145*57, "shoot buffer");
    _Static_assert(ICON32_PB_MAX_PIXELS==138*44, "pb buffer");
    for (size_t j=0; j<sizeof(targets)/sizeof(targets[0]); j++) {
        const uint32_t *id=targets[j];
        Icon32Asset pb=icon32_pb_select(id[0],id[1],id[2]);
        Icon32Asset shoot=icon32_shoot_select(id[0],id[1],id[2]);
        fwrite(&pb.ink_mid2,sizeof(pb.ink_mid2),1,stdout);
        fwrite(&shoot.ink_mid2,sizeof(shoot.ink_mid2),1,stdout);
        if (pb.w!=pb.dw || pb.h!=pb.dh || shoot.w!=shoot.dw || shoot.h!=shoot.dh)
            return 1;
        uint8_t pixel[4];
        for (uint32_t i=0; i<pb.w*pb.h; i++) {
            icon32_pb_rgba(&pb,i,pixel);
            fwrite(pixel,1,4,stdout);
        }
        for (uint32_t f=0; f<=ICON32_PHASES; f++)
            for (uint32_t i=0; i<shoot.w*shoot.h; i++) {
                icon32_shoot_rgba(&shoot,i,f ? f-1 : 0,f!=0,pixel);
                fwrite(pixel,1,4,stdout);
            }
    }
    return ferror(stdout);
}
"""
    sizes = [(88,34),(145,57),(art.ART_WIDTH,44),(art.ART_WIDTH,44),(art.ART_WIDTH,44),(88,34)]
    with tempfile.TemporaryDirectory(prefix='live-target-assets-') as tmp:
        out = Path(tmp)
        (out/'test.c').write_text(source)
        args = [sys.executable, '-B', HERE/'icon32_generate.py', '--output-dir', out,
                '--shoot-target', '7,1,4,88,34', '--shoot-target', '7,1,5,145,57',
                '--pb-target', '7,1,4,88,34']
        for binary in (False, True):
            subprocess.run(args + (['--binary'] if binary else []),check=True,stdout=subprocess.DEVNULL)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',out/'test.c','-o',out/'check'],check=True)
            pixels = {shape: art.artwork(*shape,binary=binary) for shape in set(sizes)}
            expected = bytearray()
            for w,h in sizes:
                pw,ph = (88,34) if (w,h)==(88,34) else (art.ART_WIDTH,44)
                for sw,sh in ((pw,ph),(w,h)):
                    x0,_,x1,_ = bright_bounds(bytes((pair>>4)*17 for pair in pixels[sw,sh]),sw,(0,0,sw,sh),1)
                    expected.extend(struct.pack('=H',x0+x1))
                expected.extend(art.rgba_frame(pixels[pw,ph],pw))
                expected.extend(art.rgba_frame(pixels[w,h],w))
                for phase in range(art.FRAMES):
                    expected.extend(art.rgba_frame(pixels[w,h],w,phase,binary))
            actual = subprocess.check_output([out/'check'])
            if actual != expected:
                raise ValueError('target selector or dynamic pixel stream mismatch')
        duplicate = subprocess.run(args + ['--shoot-target','7,1,4,100,40'],capture_output=True)
        if duplicate.returncode == 0:
            raise ValueError('duplicate target should be rejected')
    print('target masks: two matching shoots, one matching playback, three partial mismatches, return; AA and binary passed')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ffmpeg', default=shutil.which('ffmpeg') or '/Applications/Downie 4.app/Contents/Resources/ffmpeg')
    p.add_argument('--assets', type=Path, default=HERE/'icon32-assets', help='output directory from icon32_generate.py')
    p.add_argument('--alignment', type=Path, help='write a 2x nearest-neighbour crop of the three shooting rows')
    p.add_argument('--preview', type=Path, help='write a comparison of cropped QEMU screens at original pixel scale')
    p.add_argument('--assets-only', action='store_true', help='check only images, no MAIN inspection or C compilation')
    p.add_argument('--comparison', type=Path, help='write old/second/third style sheet and a separate 2x PNG')
    args = p.parse_args()
    binary = args.ffmpeg
    if not args.assets_only:
        fw = pinned(HERE/'bg34.main')
        # The live-view selector loads image IDs 0xa7d / 0xa7e into w3 / w4.
        if fw[0x4041a860-BASE:0x4041a868-BASE] != bytes.fromhex('a34f8152c44f8152'):
            raise ValueError('live-view image selector differs')
        for ident in (0xa7d, 0xa7e):
            row = 0x431d96d0-BASE + 24*ident
            size = struct.unpack_from('<I', fw, row+4)[0]
            at = struct.unpack_from('<Q', fw, row+16)[0] - BASE
            data = fw[at:at+size]
            w,h = dimensions(data)
            grey = decode(binary, data, 'gray')
            if len(grey) != w*h:
                raise ValueError('bad resource length')
            print(f'stock PNG ID {ident:#x}: VA={at+BASE:#x} bytes={size} pixels={w}x{h} grey_levels={len(set(grey))}')
    
    stage = {}
    stock_centers = {}
    for which, filename, rois in [
        ('shoot', 'before-playback-shooting.png', [('A',(0,0,85,68)), ('VIEW+camera',(0,65,115,130)),
                                                  ('S box',(168,5,228,61)), ('MF',(278,5,346,62)),
                                                  ('VIEW',(4,72,65,121)), ('FX',(961,72,1021,124)), ('FINE F',(796,72,824,120)),
                                                  ('FINE I',(824,72,836,120)), ('FINE E',(868,72,890,120))]),
        ('pb', 'pb-no-live.png', [('2/2',(947,0,1024,50)), ('FX',(656,635,708,682))])]:
        data = pinned(ARCHIVE/filename)
        w,h = dimensions(data)
        if (w,h) != (1024,682):
            raise ValueError('unexpected archive dimensions')
        rgb = bytearray(decode(binary, data, 'rgb24'))
        for label,roi in rois:
            a,b,c,d = bright_bounds(rgb,w,roi,3)
            if which == 'shoot' and label in ('A','VIEW+camera'):
                stock_centers[label] = ((a+c)/2,(b+d)/2)
            print(f'{which} {label} at bright RGB>=224: {c-a}x{d-b} [{a},{b},{c},{d})')
        stage[which] = rgb

    assets = args.assets
    cfg = json.loads((assets/'geometry.json').read_text())
    for prefix, scene in [('shoot',(720,480)), ('pb',(1280,800))]:
        g = cfg[prefix]
        data = (assets/(prefix+'.png')).read_bytes()
        rgba = decode(binary,data,'rgba')
        alpha = rgba[3::4]
        pixels = bytes((rgba[4*i]*a+127)//255 for i,a in enumerate(alpha))
        black = bytes(((255-rgba[4*i])*a+127)//255 for i,a in enumerate(alpha))
        visible = bytes(255 if a else 0 for a in alpha)
        contour_bounds = bright_bounds(visible,g['width'],(0,0,g['width'],g['height']),1)
        w,h = dimensions(data)
        if (w,h) != (g['width'],g['height']) or len(pixels) != w*h:
            raise ValueError('asset/config mismatch')
        expected = art.rgba_frame(art.artwork(w,h,cfg['raster']=='binary'),w)
        if data[25] != 6 or rgba != expected:
            raise ValueError('PNG requires exact straight-alpha RGBA')
        vector = ET.parse(assets/(prefix+'.svg')).getroot()
        if (vector.get('viewBox') != f'{art.VIEW_LEFT} 0 {art.ART_WIDTH} {art.ART_HEIGHT}' or vector.get('width') != str(w)
                or vector.get('height') != str(h) or vector.findall('.//{*}rect')):
            raise ValueError('SVG size/background differs')
        a,b,c,d = bright_bounds(pixels,w,(0,0,w,h),1)
        print(f'{prefix} asset {w}x{h}: bright ink {c-a}x{d-b}, alpha_levels={len(set(alpha))}, ' +
              f'partial_alpha={sum(0<v<255 for v in alpha)}')
        print(f'{prefix} nonzero-alpha outer contour ROI {contour_bounds}, opaque black count={sum(v==255 for v in black)}')
        if cfg['raster']!='binary' and w==art.ART_WIDTH and h==44:
            if any(alpha[:w]+alpha[-w:]+alpha[::w]+alpha[w-1::w]):
                raise ValueError('outline reaches canvas boundary')
            if not any(v==255 for v in black) or abs((contour_bounds[0]+contour_bounds[2])-(a+c))>2:
                raise ValueError('black outline missing or not aligned with foreground')
        if w==art.ART_WIDTH and h==44 and cfg['raster']!='binary':
            for name,box in [('ring',(0,0,40,44)),('L',(40,0,60,44)),('I',(60,0,68,44)),
                             ('V',(68,0,88,44)),('E',(88,0,110,44))]:
                l,t,r,bottom = bright_bounds(pixels,w,box,1)
                print(f'{prefix} {name} white ink {r-l}x{bottom-t}')
                if bottom-t!=34:
                    raise ValueError('symbol/letter does not match 34 px design cap height')
            # Independently check white runs through all three concentric levels.
            from icon32_asset_check import runs
            radial=runs(x for x in range(40) if pixels[22*w+x]>=224)
            if [r-l for l,r in radial] != [3,5,6,5,3]:
                raise ValueError(f'outer/inner/dot widths differ: {radial}')
            # At radius 15.5, count separated visible outer arcs and measure their
            # actual pixel support. This detects a solid circle or tiny dot ring.
            samples=[]
            for degree in range(360):
                angle=math.radians(degree)
                x=int(20+15.5*math.cos(angle)); y=int(22+15.5*math.sin(angle))
                samples.append(pixels[y*w+x]>=224)
            starts=[i for i in range(360) if samples[i] and not samples[i-1]]
            spans=[]
            for start in starts:
                length=0
                while samples[(start+length)%360]: length+=1
                spans.append(length)
            if len(starts)!=12 or min(spans)<10 or max(spans)>22:
                raise ValueError(f'outer arcs lost or merged: {spans}')
            print(f'{prefix} concentric widths outer/inner/dot/inner/outer=3/5/6/5/3; 12 distinct arcs, sampled spans={spans}')
        black_core = [i for i,v in enumerate(black) if v==255]
        white_core = [i for i,v in enumerate(pixels) if v==255]
        empty = [i for i,v in enumerate(alpha) if not v]
        if not (black_core and white_core and empty):
            raise ValueError('expected opaque white and black plus clear space')
        for bg in (12,245):
            composed = bytearray([bg])*(w*h*3)
            stamp(composed,w,0,0,rgba,w,h)
            if (any(composed[3*i] != bg for i in empty) or
                any(composed[3*i] != 0 for i in black_core) or
                any(composed[3*i] != 255 for i in white_core)):
                raise ValueError('stroke/background composite mismatch')
        # The archived framebuffer alone has no saved viewport. This preview
        # explicitly assumes a full 1024x682 viewport with the post-change 1:1 badge.
        if args.preview or args.alignment:
            if (g['dest_width'],g['dest_height']) != (w,h):
                raise ValueError('1:1 preview requires equal source/destination sizes')
            x,y = g['x']*1024//scene[0], g['y']*682//scene[1]
            if g.get('center_x') is not None:
                x = g['center_x'] - (a+c+1)//2
            x,y = max(0,min(x,1024-w)), max(0,min(y,682-h))
            stamp(stage[prefix],1024,x,y,rgba,w,h)
            print(f'preview {prefix} bright centre=({x+(a+c)/2:g},{y+(b+d)/2:g}) x={x} y={y}')
    # Rendering at four times the dimensions, then averaging each 4x4 block,
    # supplies a ~16x16 subpixel area-coverage reference for final-size pixels.
    w,h = cfg['shoot']['width'], cfg['shoot']['height']
    large = bytes((pair>>4)*17 for pair in art.artwork(w*4,h*4))
    reference = []
    for y in range(h):
        for x in range(w):
            offset = (y*4*w*4) + x*4
            reference.append(sum(sum(large[i:i+4]) for i in range(offset,offset+16*w,4*w))/16)
    edge = [i for i,v in enumerate(reference) if 0<v<255]
    errors = []
    for mode in (False, True):
        pixels = bytes((pair>>4)*17 for pair in art.artwork(w,h,binary=mode))
        rmse = math.sqrt(sum((pixels[i]-reference[i])**2 for i in edge)/len(edge))
        errors.append(rmse)
    if errors[0] >= errors[1]:
        raise ValueError('AA failed to improve edge coverage against the fine grid')
    print(f'white edge pixels={len(edge)} coverage RMSE (0..255), 4-bit={errors[0]:.2f}, binary={errors[1]:.2f}')

    frames = decode(binary,assets/'shoot-busy.gif','rgba')
    g = cfg['shoot']
    w,h = g['width'],g['height']
    size = w*h*4
    if len(frames) != art.FRAMES*size:
        raise ValueError('GIF frame count mismatch')
    pixels = art.artwork(w,h,cfg['raster']=='binary')
    animated = ET.parse(assets/'shoot-busy.svg').getroot()
    motion = animated.find('.//{*}animateTransform')
    if (motion is None or motion.get('dur') != '1.44s' or motion.get('repeatCount') != 'indefinite'
            or motion.get('calcMode') != 'discrete' or len(motion.get('values').split(';')) != art.FRAMES
            or animated.findall('.//{*}rect')):
        raise ValueError('SVG transparent sweep timing/geometry mismatch')
    for phase in range(art.FRAMES):
        actual = frames[phase*size:(phase+1)*size]
        rgba = art.rgba_frame(pixels,w,phase,cfg['raster']=='binary')
        indexed = art.gif_indices(rgba)
        if any(rgba[4*i] != 0 for i,pair in enumerate(pixels) if pair and pair>>4==0):
            raise ValueError('sweep illuminated the black outline')
        if phase and rgba[3::4] != art.rgba_frame(pixels,w)[3::4]:
            raise ValueError('sweep changed silhouette or outline alpha')
        for i, index in enumerate(indexed):
            px = actual[4*i:4*i+4]
            grey = round((index-1)*255/254) if index else 0
            if (index and px != bytes((grey,grey,grey,255))) or (not index and px[3] != 0):
                raise ValueError(f'GIF palette/alpha mismatch: {phase}, pixel {i}')
        png = decode(binary,assets/f'shoot-busy-{phase:02d}.png','rgba')
        if png != rgba:
            raise ValueError(f'PNG alpha/colour mismatch: {phase}')
    print(f'SVG: transparent 24-step 1.44s loop; GIF: {art.FRAMES} transparent phases; PNG: full RGBA match')
    if not args.assets_only:
        check_target_assets()
    def save_rgb(path, pixels, size, filter_args=()):
        cmd = [binary,'-v','error','-f','rawvideo','-pix_fmt','rgb24','-s',size,
               '-i','pipe:0',*filter_args,'-frames:v','1','-f','image2pipe','-c:v','png','pipe:1']
        path.write_bytes(subprocess.run(cmd,input=pixels,capture_output=True,check=True).stdout)
    from icon32_asset_check import measure_and_compare
    measure_and_compare(binary, assets, args.comparison)
    if args.preview:
        # Retain unscaled rows from the top of both 1024x682 scenes.
        rows = stage['shoot'][:1024*196*3] + bytes([24])*(1024*8*3) + stage['pb'][:1024*76*3]
        save_rgb(args.preview, rows, '1024x280')
    if args.alignment:
        save_rgb(args.alignment, stage['shoot'], '1024x682',
                 ('-vf', 'crop=350:196:0:0,scale=700:392:flags=neighbor'))

    print('PASS icon32_visual_check.py: ' + ('image-only scope; no MAIN or C checks' if args.assets_only else 'full visual scope'))


if __name__ == '__main__':
    main()
