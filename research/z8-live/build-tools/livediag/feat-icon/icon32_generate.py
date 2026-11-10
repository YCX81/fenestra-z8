#!/usr/bin/env python3
"""Generate the Z8 Live monochrome badge at the requested display-pixel size."""
import argparse
import json
import math
import struct
import zlib
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent
VIEW_LEFT = 0
ART_WIDTH = 110
ART_HEIGHT = 44
FRAMES = 24
FRAME_MS = 60
BAYER = (0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5)


def png(path, width, height, pixels):
    """Write straight RGBA, retaining fractional white-stroke coverage."""
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))

    raw = b''.join(b'\0' + pixels[y*width*4:(y+1)*width*4] for y in range(height))
    header = struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def gif_indices(rgba):
    # GIF has one transparent palette index. Threshold *only* the edge alpha;
    # SVG and PNG keep the true partial coverage. Sweep brightness stays grey.
    return bytes(1+round(rgba[i]*254/255) if rgba[i+3] >= 128 else 0 for i in range(0, len(rgba), 4))


def gif(path, width, height, frames):
    # Full grey palette. Clear before the LZW dictionary reaches 512 entries,
    # so literals, clear and end codes are always exactly nine bits wide.
    data = bytearray(b'GIF89a' + struct.pack('<HHBBB', width, height, 0xf7, 0, 0))
    data += bytes([0,0,0]) + bytes(round(grey*255/254) for grey in range(255) for _ in range(3))
    data += b'\x21\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00'
    for rgba in frames:
        pixels = gif_indices(rgba)
        data += b'\x21\xf9\x04\x09' + struct.pack('<H', FRAME_MS // 10) + b'\0\0'
        data += b'\x2c' + struct.pack('<HHHHBB', 0, 0, width, height, 0, 8)
        packed = bytearray()
        bits = count = 0

        def put(code):
            nonlocal bits, count
            bits |= code << count
            count += 9
            while count >= 8:
                packed.append(bits & 255)
                bits >>= 8
                count -= 8

        for start in range(0, len(pixels), 250):
            put(256)
            for pixel in pixels[start:start+250]:
                put(pixel)
        put(257)
        if count:
            packed.append(bits)
        for start in range(0, len(packed), 255):
            block = packed[start:start+255]
            data.append(len(block))
            data += block
        data.append(0)
    path.write_bytes(data + b';')


SVG_NS = 'http://www.w3.org/2000/svg'
ET.register_namespace('', SVG_NS)


def vector_tree():
    return ET.parse(ROOT/'live-badge.svg').getroot()


def write_svg(path, width, height, animated=False):
    root = vector_tree()
    root.set('width', str(width))
    root.set('height', str(height))
    if animated:
        el = lambda tag: f'{{{SVG_NS}}}{tag}'
        defs = ET.Element(el('defs'))
        root.insert(1, defs)
        gradient = ET.SubElement(defs, el('linearGradient'), id='sweep', gradientUnits='userSpaceOnUse',
                                 x1=str(-ART_WIDTH/2), y1='0', x2=str(ART_WIDTH/2), y2='0')
        # Linear interpolation of the same Gaussian as the firmware. Outside
        # the gradient the endpoint value is extended by SVG's pad behaviour.
        for step in range(57):
            dx = (step/56-.5)*ART_WIDTH
            value = round(64 + 191*math.exp(-(dx/(ART_WIDTH*22/112))**2))
            ET.SubElement(gradient, el('stop'), offset=f'{step/56:.8f}',
                          attrib={'stop-color': f'rgb({value},{value},{value})'})
        margin = width * 30 // 112
        centers = [f'{(i*(width+2*margin)//FRAMES-margin+0.5)*ART_WIDTH/width+VIEW_LEFT:.8f} 0'
                   for i in range(FRAMES)]
        gradient.set('gradientTransform', f'translate({centers[0]})')
        ET.SubElement(gradient, el('animateTransform'), attributeName='gradientTransform', type='translate',
                      dur=f'{FRAMES*FRAME_MS/1000:.2f}s', repeatCount='indefinite', calcMode='discrete',
                      values=';'.join(centers))
        for shape in root.iter():
            for key in ('fill','stroke'):
                if shape.get(key) == '#fff':
                    shape.set(key, 'url(#sweep)')
    ET.indent(root, '  ')
    path.write_text(ET.tostring(root, encoding='unicode')+'\n')


def artwork(width, height, binary=False):
    # Work in the source SVG artwork coordinates. For each primitive,
    # visit only its bounding rectangle in the 4 x 4 subpixel coverage grid.
    samples = 4
    cols, rows = width*samples, height*samples
    xs = [(i+.5)*ART_WIDTH/cols + VIEW_LEFT for i in range(cols)]
    ys = [(i+.5)*ART_HEIGHT/rows for i in range(rows)]
    mask = bytearray(cols*rows)  # 0 transparent, 1 black, 2 white
    colour = 0

    def paint(x0, y0, x1, y1, inside):
        left, right = max(0, math.floor((x0-VIEW_LEFT)*cols/ART_WIDTH)), min(cols, math.ceil((x1-VIEW_LEFT)*cols/ART_WIDTH))
        first, last = max(0, int(y0*rows/ART_HEIGHT)), min(rows, math.ceil(y1*rows/ART_HEIGHT))
        for j in range(first, last):
            row = j*cols
            for i in range(left, right):
                if inside(xs[i], ys[j]):
                    mask[row+i] = colour

    def ring(cx, cy, r, hole=0):
        paint(cx-r, cy-r, cx+r, cy+r, lambda x,y: hole**2 <= (x-cx)**2+(y-cy)**2 <= r*r)

    def stroke(a, b, c, d, radius):
        dx, dy = c-a, d-b
        length2 = dx*dx+dy*dy
        def inside(x, y):
            t = min(1, max(0, ((x-a)*dx+(y-b)*dy)/length2))
            return (x-a-dx*t)**2+(y-b-dy*t)**2 <= radius**2
        paint(min(a,c)-radius, min(b,d)-radius, max(a,c)+radius, max(b,d)+radius, inside)

    def polygon(points):
        xmin,ymin = map(min,zip(*points))
        xmax,ymax = map(max,zip(*points))
        edges = list(zip(points,points[1:]+points[:1]))
        def inside(x,y):
            return sum((ay>y)!=(by>y) and x<(bx-ax)*(y-ay)/(by-ay)+ax
                       for (ax,ay),(bx,by) in edges)%2
        paint(xmin,ymin,xmax,ymax,inside)

    # The editable SVG is the master. Accept its elementary circles/rings and
    # round-ended lines, and fail if a revised design needs a new primitive.
    def supported(node, attributes):
        extras = node.attrib.keys() - set(attributes)
        if extras:
            raise ValueError(f'unsupported SVG attributes on {node.tag}: {sorted(extras)}')

    root = vector_tree()
    supported(root, ('width','height','viewBox','preserveAspectRatio'))
    if root.get('preserveAspectRatio') != 'none':
        raise ValueError('firmware raster expects preserveAspectRatio="none"')
    if root.get('viewBox') != f'{VIEW_LEFT} 0 {ART_WIDTH} {ART_HEIGHT}':
        raise ValueError('unsupported artwork viewBox')
    for group in root:
        if group.tag == f'{{{SVG_NS}}}title':
            continue
        if group.tag != f'{{{SVG_NS}}}g':
            raise ValueError(f'unsupported SVG group {group.tag}')
        styles = ('id','fill','stroke','stroke-width','stroke-linecap','stroke-linejoin')
        supported(group, styles)
        for shape in group:
            attrs = {**group.attrib, **shape.attrib}
            kind = shape.tag.removeprefix(f'{{{SVG_NS}}}')
            if kind == 'circle':
                supported(shape, (*styles,'cx','cy','r'))
                cx,cy,r = (float(attrs[k]) for k in ('cx','cy','r'))
                if attrs.get('fill') in ('#fff','#000') and attrs.get('stroke','none') == 'none':
                    colour = 2 if attrs['fill'] == '#fff' else 1
                    ring(cx,cy,r)
                elif attrs.get('fill') == 'none' and attrs.get('stroke') in ('#fff','#000'):
                    colour = 2 if attrs['stroke'] == '#fff' else 1
                    half = float(attrs['stroke-width'])/2
                    ring(cx,cy,r+half,max(0,r-half))
                else:
                    raise ValueError('unsupported SVG circle style')
            elif kind == 'polygon' and attrs.get('fill') in ('#000','#fff'):
                supported(shape, (*styles,'points'))
                points = [tuple(map(float,p.split(','))) for p in attrs['points'].split()]
                colour = 2 if attrs['fill']=='#fff' else 1
                polygon(points)
                if attrs.get('stroke','none')!='none':
                    if attrs['stroke']!=attrs['fill'] or attrs.get('stroke-linejoin')!='round':
                        raise ValueError('unsupported polygon stroke style')
                    for start,end in zip(points,points[1:]+points[:1]):
                        stroke(*start,*end,float(attrs['stroke-width'])/2)
            elif (kind == 'line' and attrs.get('stroke') in ('#fff','#000')
                  and attrs.get('stroke-linecap') == 'round'):
                supported(shape, (*styles,'x1','y1','x2','y2'))
                colour = 2 if attrs['stroke'] == '#fff' else 1
                stroke(*(float(attrs[k]) for k in ('x1','y1','x2','y2')), float(attrs['stroke-width'])/2)
            else:
                raise ValueError(f'unsupported SVG primitive/style {kind}')
    pixels = bytearray(width*height)
    for y in range(height):
        for x in range(width):
            start = y*samples*cols + x*samples
            opaque = white = 0
            for pos in range(start,start+samples*cols,cols):
                grid = mask[pos:pos+samples]
                opaque += samples - grid.count(0)
                white += grid.count(2)
            if binary:
                a, ink = (15 if opaque>=8 else 0), (15 if white>=8 else 0)
            else:
                a, ink = (opaque*15+8)//16, (white*15+8)//16
            pixels[y*width+x] = (ink<<4) | a
    return pixels


def sweep(width, binary):
    base, amplitude = (4,12) if binary else (64,191)
    return [round(base + amplitude*math.exp(-(dx/(width*22/112))**2)) for dx in range(width//2+1)]


def rgba_frame(coverage, width, phase=None, binary=False):
    """Straight RGBA. Keep the luminance function independent of antialias alpha."""
    rgba = bytearray(4*len(coverage))
    levels = sweep(width,binary)
    margin = width*30//112
    center = 0 if phase is None else phase*(width+2*margin)//FRAMES-margin
    for i, pair in enumerate(coverage):
        alpha = pair & 15
        white = pair >> 4
        if not alpha:
            continue
        value = 255
        if phase is not None:
            y, x = divmod(i,width)
            dist = abs(x-center)
            gain = levels[dist] if dist < len(levels) else (4 if binary else 64)
            if binary:
                if BAYER[(y%4)*4+x%4] >= gain:
                    value = 0
            else:
                value = gain
        grey = (value*white + alpha//2)//alpha
        rgba[i*4:i*4+4] = bytes((grey,grey,grey,alpha*17))
    return rgba


def array(name, values, ctype="uint8_t"):
    lines = [','.join(map(str, values[i:i+32]))+',' for i in range(0, len(values), 32)]
    return f'static const {ctype} {name}[{len(values)}]={{\n' + '\n'.join(lines) + '\n};\n'


def packed_artwork(images):
    # One palette per owner, indexed in constant time. Sixteen horizontal
    # pixels form a reusable block; identical rows reuse the same block list.
    # Padding is local to each row, so a partial last block never reads across it.
    palette = sorted({0} | {value for pixels in images.values() for value in pixels})
    codes = {value:index for index,value in enumerate(palette)}
    bits = max(1,(len(palette)-1).bit_length())
    blocks, row_offsets = {}, {}
    block_ids, rows, offsets = [], [], {}
    for (w,h), pixels in images.items():
        offsets[w,h] = len(rows)
        for y in range(h):
            row = []
            for x in range(0,w,16):
                group = bytes(pixels[y*w+x:y*w+min(w,x+16)]).ljust(16,b'\0')
                row.append(blocks.setdefault(group,len(blocks)))
            key = tuple(row)
            if key not in row_offsets:
                row_offsets[key] = len(block_ids)
                block_ids.extend(row)
            rows.append(row_offsets[key])
    packed = bytearray()
    for block in blocks:
        value = sum(codes[pixel] << (i*bits) for i,pixel in enumerate(block))
        packed.extend(value.to_bytes(2*bits,'little'))
    packed.append(0)  # bounded two-byte read at the last packed code
    return palette,bits,packed,block_ids,rows,offsets


def index_type(values):
    m = max(values)
    return 'uint8_t' if m<=255 else 'uint16_t' if m<=65535 else 'uint32_t'


def target_override(text):
    try:
        device, kind, target, w, h = [int(word, 0) for word in text.split(',')]
    except ValueError as exc:
        raise argparse.ArgumentTypeError('expected device,type,target,width,height') from exc
    if any(not 0 <= value <= 0xffffffff for value in (device,kind,target)):
        raise argparse.ArgumentTypeError('target tuple requires three unsigned 32-bit values')
    if not (64 <= w <= 512 and 24 <= h <= 256):
        raise argparse.ArgumentTypeError('target size must fit 64..512 × 24..256 pixels')
    return device,kind,target,w,h


def c_artwork(prefix, geom, images, targets, binary):
    pre, stem = 'ICON32_'+prefix.upper(), 'icon32_'+prefix
    header = f'#ifndef {pre}_H\n#define {pre}_H\n#include <stdint.h>\n'
    # Store only numeric offsets in each style. Generated payloads do not
    # introduce pointer-bearing data records with load-address relocations.
    header += ('#ifndef ICON32_ASSET_DEFINED\n#define ICON32_ASSET_DEFINED\n'
               'typedef struct { uint16_t w,h,x,y,dw,dh; uint32_t mask,gain; uint16_t ink_mid2; } Icon32Asset;\n#endif\n')
    for key, value in geom.items():
        if key == 'center_x' and value is None:
            value = 0xffffffff
        header += f'#define {pre}_{key.upper()} {value}u\n'
    header += f'#define {pre}_BINARY {int(binary)}u\n#define {pre}_HAS_TARGETS {int(bool(targets))}u\n'
    header += f'#define {pre}_MAX_PIXELS {max(w*h for w,h in images)}u\n'
    palette,bits,tiles,indices,rows,offset = packed_artwork(images)
    gains, gain_offset = bytearray(), {}
    for w,h in images:
        if prefix=='shoot' and w not in gain_offset:
            gain_offset[w] = len(gains)
            gains.extend(sweep(w,binary))
    header += array(stem+'_palette',palette)
    header += array(stem+'_tiles',tiles)
    header += array(stem+'_index',indices,index_type(indices))
    header += array(stem+'_rows',rows,index_type(rows))
    def descriptor(w,h,dw,dh):
        bright = [x for x in range(w) if any((pair>>4)*17>=224 for pair in images[w,h][x::w])]
        if not bright:
            raise ValueError('empty bright ink cannot be centre-aligned')
        ink_mid2 = min(bright) + max(bright) + 1
        return ('(Icon32Asset){' + ','.join(str(v)+'u' for v in
                (w,h,geom['x'],geom['y'],dw,dh,offset[w,h],gain_offset.get(w,0),ink_mid2)) + '}')
    base = descriptor(geom['width'],geom['height'],geom['dest_width'],geom['dest_height'])
    header += f'static inline Icon32Asset {stem}_default(void) {{ return {base}; }}\n'
    header += f'static inline Icon32Asset {stem}_select(uint32_t device,uint32_t kind,uint32_t target) {{\n'
    if not targets:
        header += '    (void)device; (void)kind; (void)target;\n'
    for device,kind,target,w,h in targets:
        header += (f'    if(device=={device}u && kind=={kind}u && target=={target}u)\n' +
                   f'        return {descriptor(w,h,w,h)};\n')
    header += f'    return {stem}_default();\n}}\n'
    header += f"""static inline uint8_t {stem}_pixel(const Icon32Asset *a,uint32_t i) {{
    uint32_t y=i/a->w, x=i%a->w;
    uint32_t row={stem}_rows[a->mask+y];
    uint32_t tile={stem}_index[row+x/16u];
    uint32_t bit=(x%16u)*{bits}u;
    uint32_t at=tile*{2*bits}u+bit/8u;
    uint32_t packed={stem}_tiles[at]|((uint32_t){stem}_tiles[at+1u]<<8);
    return {stem}_palette[(packed>>(bit%8u))&{(1<<bits)-1}u];
}}
"""
    if prefix == 'shoot':
        header += f'#define ICON32_PHASES {FRAMES}u\n#define ICON32_FRAME_US {FRAME_MS*1000}u\n'
        header += array('icon32_gain', gains)
        if binary:
            header += array('icon32_threshold', BAYER)
        header += """static inline uint8_t icon32_shoot_gain(const Icon32Asset *a,uint32_t i,uint32_t phase) {
    const uint32_t w=a->w, margin=w*30u/112u;
    int32_t center=(int32_t)(phase*(w+2u*margin)/ICON32_PHASES)-(int32_t)margin;
    int32_t dx=(int32_t)(i%w)-center;
    uint32_t dist=(uint32_t)(dx<0?-dx:dx);
"""
        if binary:
            header += """    uint32_t gain=dist<=w/2u?icon32_gain[a->gain+dist]:4u;
    return icon32_threshold[((i/w)%4u)*4u+(i%w)%4u]<gain?255u:0u;
"""
        else:
            header += """    return dist<=w/2u?icon32_gain[a->gain+dist]:64u;
"""
        header += """}
static inline void icon32_shoot_rgba(const Icon32Asset *a,uint32_t i,uint32_t phase,int busy,uint8_t out[4]) {
    uint8_t pair=icon32_shoot_pixel(a,i), alpha=pair&15u;
    uint8_t gain=busy?icon32_shoot_gain(a,i,phase):255u;
    out[0]=out[1]=out[2]=alpha?(uint8_t)(((pair>>4)*gain+alpha/2u)/alpha):0u;
    out[3]=(uint8_t)(alpha*17u);
}
"""
    else:
        header += """static inline void icon32_pb_rgba(const Icon32Asset *a,uint32_t i,uint8_t out[4]) {
    uint8_t pair=icon32_pb_pixel(a,i), alpha=pair&15u;
    out[0]=out[1]=out[2]=alpha?(uint8_t)(((pair>>4)*255u+alpha/2u)/alpha):0u;
    out[3]=(uint8_t)(alpha*17u);
}
"""
    return header+'#endif\n'


def main():
    parser = argparse.ArgumentParser()
    for name, default in [('pb-width',ART_WIDTH), ('pb-height',44), ('pb-x',24), ('pb-y',24),
                          ('shoot-width',ART_WIDTH), ('shoot-height',44), ('shoot-x',None), ('shoot-y',98)]:
        parser.add_argument('--'+name, type=int, default=default)
    parser.add_argument('--shoot-center-x', type=int,
                        help='align full bright-ink centre in a 1024-wide reference viewport (default 54)')
    for prefix in ('pb', 'shoot'):
        parser.add_argument(f'--{prefix}-target', action='append', type=target_override, default=[],
                            metavar='DEVICE,TYPE,TARGET,W,H', help='optional size for an independently measured output tuple')
        for dimension in ('width', 'height'):
            parser.add_argument(f'--{prefix}-dest-{dimension}', type=int)
    parser.add_argument('--assets-only', action='store_true', help='write SVG/PNG/GIF/geometry only; leave C headers untouched')
    parser.add_argument('--binary', action='store_true', help='store only pure 0/255 pixels and dither busy brightness')
    parser.add_argument('--output-dir', type=Path, default=ROOT/'icon32-assets')
    args = vars(parser.parse_args())
    out = args.pop('output_dir')
    binary = args.pop('binary')
    assets_only = args.pop('assets_only')
    center_x = args['shoot_center_x']
    if args['shoot_x'] is None:
        center_x = 54 if center_x is None else center_x
        args['shoot_x'] = 24  # legacy left anchor, inactive in centre mode
    elif center_x is not None:
        parser.error('choose one of --shoot-x and --shoot-center-x')
    if center_x is not None and not 0 <= center_x <= 1024:
        parser.error('--shoot-center-x must be within the 1024-wide reference viewport')
    geometries, targets = {}, {}
    for prefix, scene in [('pb', (1280,800)), ('shoot', (720,480))]:
        get = lambda key: args[prefix+'_'+key]
        w, h, x, y = map(get, ('width', 'height', 'x', 'y'))
        dw = w if get('dest_width') is None else get('dest_width')
        dh = h if get('dest_height') is None else get('dest_height')
        if not (64 <= w <= 512 and 24 <= h <= 256 and 64 <= dw <= 512 and 24 <= dh <= 256
                and 0 <= x <= scene[0]-dw and 0 <= y <= scene[1]-dh):
            parser.error(f'{prefix}: source/destination must fit 64..512 × 24..256 and the destination must fit scene {scene}')
        targets[prefix] = get('target')
        keys = [t[:3] for t in targets[prefix]]
        if len(set(keys)) != len(keys):
            parser.error(f'{prefix}: duplicate device/type/target tuple')
        geometries[prefix] = dict(width=w, height=h, x=x, y=y, dest_width=dw, dest_height=dh)
    out.mkdir(parents=True, exist_ok=True)
    images = []
    for prefix, geom in geometries.items():
        if prefix == 'shoot':
            geom['center_x'] = center_x
        w, h = geom['width'], geom['height']
        pixels = artwork(w,h,binary)
        normal = rgba_frame(pixels,w)
        png(out/(prefix+'.png'),w,h,normal)
        write_svg(out/(prefix+'.svg'),w,h)
        frames = [rgba_frame(pixels,w,f,binary) for f in range(FRAMES)]
        for f, frame in enumerate(frames):
            png(out/f'{prefix}-busy-{f:02d}.png',w,h,frame)
        if prefix == 'shoot':
            gif(out/'shoot-busy.gif',w,h,frames)
            write_svg(out/'shoot-busy.svg',w,h,animated=True)
        bitmaps = {(w,h): pixels}
        for _,_,_,tw,th in targets[prefix]:
            if (tw,th) not in bitmaps:
                bitmaps[tw,th] = artwork(tw,th,binary)
        if not assets_only:
            (out/(prefix+'.h')).write_text(c_artwork(prefix,geom,bitmaps,targets[prefix],binary))
        images.extend([(w,h,normal), (w,h,frames[FRAMES//2])])
    width = max(w for w,_,_ in images)*3
    height = sum(h*3+12 for _,h,_ in images)
    sheet = bytearray(width*height*4)
    top = 0
    for w,h,pixels in images:
        for y in range(h*3):
            start = (y//3)*w*4
            row = b''.join(pixels[i:i+4]*3 for i in range(start,start+w*4,4))
            offset = (top+y)*width*4
            sheet[offset:offset+len(row)] = row
        top += h*3+12
    png(out/'preview.png',width,height,sheet)
    geometries['source'] = 'live-badge.svg'
    geometries['background'] = 'transparent'
    geometries['targets'] = targets
    geometries['delivery'] = 'preview-only; C headers not updated' if assets_only else 'assets-and-headers'
    geometries['design'] = dict(viewbox=[0,0,ART_WIDTH,ART_HEIGHT], cap_height=34, stem_width=5, outline_width=2, ring_width=5, outer_ring_width=3, outer_ring_segments=12, outer_arc_degrees=16, outer_arc_centerline_length=15.5*math.radians(16), outer_arc_gap=15.5*math.radians(14), symbol_text_gap=4, vector_letter_gaps=[3,3,3], bright_letter_gaps=[3,3,3], width_reason='34px caps and symbol; 17/34 condensed letters match stock FINE 19/39 within 3%; compact spacing keeps contour inside first column')
    geometries['raster'] = 'binary' if binary else '4-bit-coverage'
    (out/'geometry.json').write_text(json.dumps(geometries, indent=2)+'\n')
    print('Generated Live icons and animation in',out)


if __name__ == '__main__':
    main()
