#!/usr/bin/env python3
"""离线、无重编码的 Live MOV 修正器；不代表 Nikon 播放认证。
仅支持非 fragmented、自包含 MOV。未知盒原样保存；mdat 不变。
"""
import argparse, struct, hashlib, json
import os
import tempfile
from pathlib import Path

def u32(b, p=0): return struct.unpack_from('>I', b, p)[0]
def pack(n): return struct.pack('>I', n)
def sha(b): return hashlib.sha256(b).hexdigest()

class Box:
    def __init__(self, typ, data, start=0, header=8):
        self.typ, self.data, self.start, self.header = typ, data, start, header
        self.children = None
        self.prefix = b''
        skip = {b'stsd':8, b'avc1':78, b'hvc1':78}.get(typ, 0)
        if typ in (b'moov', b'trak', b'mdia', b'minf', b'stbl', b'edts', b'tref', b'dinf', b'stsd', b'avc1', b'hvc1'):
            self.prefix = data[:skip]
            self.children = parse(data, skip, len(data), start+header)
    def body(self):
        return self.data if self.children is None else self.prefix+b''.join(x.encode() for x in self.children)
    def encode(self):
        b=self.body()
        if self.header==16: return pack(1)+self.typ+struct.pack('>Q',len(b)+16)+b
        return pack(len(b)+8)+self.typ+b
    def all(self, typ):
        out=[self] if self.typ==typ else []
        for c in self.children or []: out+=c.all(typ)
        return out
    def one(self,typ):
        a=self.all(typ)
        if len(a)!=1: raise ValueError(f'expected one {typ}, got {len(a)}')
        return a[0]

def parse(b, start=0, end=None, base=0):
    end=len(b) if end is None else end
    out=[]; p=start
    while p<end:
        if p+8>end: raise ValueError('truncated box')
        n=u32(b,p); typ=b[p+4:p+8]; h=8
        if n==1:
            if p+16>end: raise ValueError('truncated extended box')
            n=struct.unpack_from('>Q',b,p+8)[0];h=16
        if n==0: n=end-p
        if n<h or p+n>end: raise ValueError(f'invalid box {typ} at {base+p}')
        out.append(Box(typ,b[p+h:p+n],base+p,h));p+=n
    return out

class Bits:
    def __init__(self,b): self.b=b;self.i=0
    def get(self,n):
        if self.i+n>len(self.b)*8: raise ValueError('truncated SPS')
        v=0
        for _ in range(n): v=v*2+((self.b[self.i//8]>>(7-self.i%8))&1);self.i+=1
        return v
    def ue(self):
        n=0
        while not self.get(1):
            n+=1
            if n>31: raise ValueError('invalid Exp-Golomb')
        return (1<<n)-1+self.get(n)
    def se(self):
        x=self.ue();return (x+1)//2 if x&1 else -x//2

def sps_fields(avcc):
    if len(avcc) < 9 or avcc[0] != 1 or avcc[5]&31!=1: raise ValueError('requires one SPS')
    n=int.from_bytes(avcc[6:8],'big'); nal=avcc[8:8+n]
    if not n or len(nal)!=n or nal[0]&31!=7: raise ValueError('bad SPS')
    raw=bytearray();zeros=0
    for c in nal[1:]:
        if zeros==2 and c==3: zeros=0;continue
        raw.append(c);zeros=zeros+1 if c==0 else 0
    r=Bits(raw);profile=r.get(8);r.get(16);r.ue();chroma=1;separate=0
    if profile in (100,110,122,244,44,83,86,118,128,138,139,134,135):
        chroma=r.ue()
        if chroma==3: separate=r.get(1)
        r.ue();r.ue();r.get(1)
        if r.get(1):
            for i in range(8 if chroma!=3 else 12):
                if r.get(1):
                    last=next_=8
                    for j in range(16 if i<6 else 64):
                        if next_: next_=(last+r.se()+256)%256
                        last=next_ or last
    r.ue();poc=r.ue()
    if poc==0:r.ue()
    elif poc==1:
        r.get(1);r.se();r.se()
        for _ in range(r.ue()):r.se()
    elif poc!=2:raise ValueError('bad POC')
    r.ue();r.get(1);w=(r.ue()+1)*16;hm=r.ue()+1;frame=r.get(1)
    if not frame:r.get(1)
    h=hm*16*(2-frame);r.get(1)
    crop_bit = r.i
    has_crop = r.get(1)
    crop = [r.ue() for _ in range(4)] if has_crop else [0]*4
    cat=0 if separate else chroma
    cx=1 if cat in (0,3) else 2;cy=(2 if cat==1 else 1)*(2-frame)
    geometry = {'coded':[w,h], 'display':[w-cx*(crop[0]+crop[1]),h-cy*(crop[2]+crop[3])], 'crop':crop}
    return geometry, raw, crop_bit, has_crop, (frame == 1 and cat == 1)


def sps_size(avcc):
    return sps_fields(avcc)[0]


def hevc_rbsp(nal):
    raw = bytearray()
    zeros = 0
    payload = nal[2:]
    for i, value in enumerate(payload):
        if zeros == 2 and value == 3:
            if i + 1 == len(payload) or payload[i + 1] > 3:
                raise ValueError('invalid HEVC emulation prevention')
            zeros = 0
            continue
        raw.append(value)
        zeros = zeros + 1 if value == 0 else 0
    return raw


def hevc_description(config):
    """Inspect single-layer hvc1 with one VPS/SPS/PPS and report actual PTL declarations."""
    if len(config) < 23 or config[0] != 1:
        raise ValueError('invalid hvcC')
    sets = {}
    pos = 23
    for _ in range(config[22]):
        if pos + 3 > len(config):
            raise ValueError('truncated hvcC array')
        kind = config[pos] & 63
        count = int.from_bytes(config[pos+1:pos+3], 'big')
        pos += 3
        for _ in range(count):
            if pos + 2 > len(config):
                raise ValueError('truncated hvcC NAL length')
            size = int.from_bytes(config[pos:pos+2], 'big')
            pos += 2
            nal = config[pos:pos+size]
            if (len(nal) != size or size < 3 or nal[0] >> 1 != kind
                    or nal[0] & 1 or nal[1] >> 3 or not nal[1] & 7):
                raise ValueError('invalid or multilayer hvcC NAL')
            if kind in (32, 33, 34):
                if kind in sets:
                    raise ValueError('multiple HEVC parameter sets of one type')
                sets[kind] = nal
            pos += size
    if pos != len(config) or set(sets) != {32, 33, 34}:
        raise ValueError('expected complete VPS/SPS/PPS arrays')
    raw = hevc_rbsp(sets[33])
    vps = hevc_rbsp(sets[32])
    if len(raw) < 13 or len(vps) < 16:
        raise ValueError('short general_profile_tier_level')
    # profile_space/tier/profile_idc occupy one byte. The level is 11 bytes later.
    profile_level = {'hvcC': [config[1], config[12]], 'SPS': [raw[1], raw[12]],
                     'VPS': [vps[4], vps[15]]}
    diagnostics = {
        'profile_byte_and_level_idc': profile_level,
        'matching_profile_tier_and_level': len(set(map(tuple, profile_level.values()))) == 1,
        'passes_0x40ce84ec_byte_check': config[1] in (0x21, 0x22),
    }
    bits = Bits(raw)
    bits.get(4)
    sublayers = bits.get(3)
    if sublayers > 6:
        raise ValueError('invalid HEVC sublayer count')
    bits.get(1)
    bits.get(96)  # general_profile_tier_level
    flags = [(bits.get(1), bits.get(1)) for _ in range(sublayers)]
    if sublayers:
        bits.get(2 * (8 - sublayers))
    for profile, level in flags:
        if profile:
            bits.get(88)
        if level:
            bits.get(8)
    bits.ue()  # sps_seq_parameter_set_id
    chroma = bits.ue()
    if chroma > 3:
        raise ValueError('invalid chroma_format_idc')
    if chroma == 3:
        bits.get(1)
    w, h = bits.ue(), bits.ue()
    crop = [bits.ue() for _ in range(4)] if bits.get(1) else [0] * 4
    display = [w - (2 if chroma in (1, 2) else 1) * sum(crop[:2]),
               h - (2 if chroma == 1 else 1) * sum(crop[2:])]
    if min(display) <= 0:
        raise ValueError('invalid HEVC conformance window')
    return {'coded': [w, h], 'display': display, 'crop': crop}, diagnostics


def video_description(track):
    entries = track.one(b'stsd').children
    if len(entries) != 1 or entries[0].typ not in (b'avc1', b'hvc1'):
        raise ValueError('expected one avc1 or hvc1 video sample description')
    entry = entries[0]
    if entry.typ == b'hvc1':
        config = entry.one(b'hvcC').data
        geometry, diagnostics = hevc_description(config)
        return entry, geometry, (config[21] & 3) + 1, diagnostics
    config = entry.one(b'avcC').data
    return entry, sps_size(config), (config[4] & 3) + 1, None


def crop_avcc(avcc, width, height):
    geometry, raw, crop_bit, has_crop, progressive_420 = sps_fields(avcc)
    coded_w, coded_h = geometry['coded']
    bottom = coded_h - height
    if (not progressive_420 or coded_w != width or not 0 < bottom < 16
            or height <= 0 or bottom % 2 or not raw[-1]):
        raise ValueError('requires progressive 4:2:0 and only even bottom padding in final macroblock')
    crop = [0, 0, 0, bottom // 2]
    if has_crop:
        if geometry['crop'] == crop:
            return avcc
        raise ValueError('SPS already has a different crop')
    # Retain every bit up to and including rbsp_stop_one_bit, including VUI.
    original = ''.join(f'{value:08b}' for value in raw).rstrip('0')
    bottom_code = f'{crop[3] + 1:b}'
    encoded_crop = '1111' + '0' * (len(bottom_code) - 1) + bottom_code
    bits = original[:crop_bit] + encoded_crop + original[crop_bit + 1:]
    bits += '0' * (-len(bits) % 8)
    rbsp = int(bits, 2).to_bytes(len(bits) // 8, 'big')
    nal = bytearray([avcc[8]])
    zeros = 0
    for value in rbsp:
        if zeros == 2 and value <= 3:
            nal.append(3)
            zeros = 0
        nal.append(value)
        zeros = zeros + 1 if value == 0 else 0
    old_length = int.from_bytes(avcc[6:8], 'big')
    return avcc[:6] + len(nal).to_bytes(2, 'big') + nal + avcc[8 + old_length:]

def handler(t): return t.one(b'hdlr').data[8:12]
def offsets(t):
    a=t.all(b'stco')+t.all(b'co64')
    if len(a)!=1:raise ValueError('requires one chunk offset table')
    x=a[0];step=4 if x.typ==b'stco' else 8;n=u32(x.data,4)
    if len(x.data)!=8+n*step:raise ValueError('offset table length')
    return x,[int.from_bytes(x.data[8+i*step:8+(i+1)*step],'big') for i in range(n)]

def inspect(blob, reject_inband_sps=False):
    roots=parse(blob); moov=next(x for x in roots if x.typ==b'moov');mdats=[x for x in roots if x.typ==b'mdat']
    mdat_ranges=[(x.start+x.header,x.start+x.header+len(x.data)) for x in mdats]
    result={'bytes':len(blob),'sha256':sha(blob),'top':[(x.typ.decode(),x.start,len(x.encode())) for x in roots], 'tracks':[]}
    for t in moov.all(b'trak'):
        kind=handler(t);co,offs=offsets(t);sz=t.one(b'stsz').data;fixed=u32(sz,4);count=u32(sz,8)
        if len(sz)!=12+(0 if fixed else count*4):raise ValueError('stsz length')
        sc=t.one(b'stsc').data;n=u32(sc,4)
        if len(sc)!=8+n*12 or not n:raise ValueError('stsc length')
        entries=[struct.unpack_from('>III',sc,8+12*i) for i in range(n)]
        if entries[0][0]!=1 or any(a[0]>=b[0] for a,b in zip(entries,entries[1:])):raise ValueError('stsc ordering')
        if not offs or entries[-1][0] > len(offs):
            raise ValueError('sample-to-chunk entry outside chunk table')
        view = memoryview(blob)
        hashes = hashlib.sha256()
        idx = 0
        entry_index = 0
        total_bytes = 0
        sample_data = []
        for ci,off in enumerate(offs,1):
            if entry_index + 1 < len(entries) and entries[entry_index + 1][0] == ci:
                entry_index += 1
            entry = entries[entry_index]
            if entry[2]!=1 or entry[1]<1:raise ValueError('unsupported sample description')
            end = idx + entry[1]
            if end > count:
                raise ValueError('sample overflow')
            if fixed:
                chunk_size = fixed * entry[1]
            else:
                chunk_size = sum(u32(sz, 12 + 4*i) for i in range(idx, end))
            if not any(lo <= off and off + chunk_size <= hi for lo, hi in mdat_ranges):
                raise ValueError('chunk outside mdat')
            hashes.update(view[off:off + chunk_size])
            total_bytes += chunk_size
            if kind == b'vide':
                for i in range(idx, end):
                    size = fixed or u32(sz, 12 + 4*i)
                    sample_data.append(view[off:off + size])
                    off += size
            idx = end
        if idx!=count:raise ValueError('sample count mismatch')
        ts=t.one(b'stts').data;nt=u32(ts,4)
        if len(ts)!=8+nt*8 or sum(u32(ts,8+i*8) for i in range(nt))!=count:raise ValueError('stts mismatch')
        item={'handler':kind.decode(),'offset_type':co.typ.decode(),'chunks':len(offs),'samples':count,'sample_bytes':total_bytes,'sample_sha256':hashes.hexdigest(),'first_offset':offs[0]}
        if kind==b'vide':
            entry, item['sps'], width, diagnostics = video_description(t)
            if diagnostics is not None:
                item['hevc'] = diagnostics
            item[entry.typ.decode() + '_size'] = list(struct.unpack_from('>HH', entry.prefix, 24))
            item['tkhd_size']=[u32(t.one(b'tkhd').data,len(t.one(b'tkhd').data)-8+i*4)/65536 for i in range(2)]
            for data in sample_data:
                p=0
                while p<len(data):
                    if p+width>len(data):raise ValueError('truncated NAL size')
                    n=int.from_bytes(data[p:p+width],'big');p+=width
                    if n==0 or p+n>len(data):raise ValueError('bad NAL bounds')
                    if entry.typ == b'hvc1' and (n < 2 or data[p] & 0x80 or not data[p+1] & 7
                                                   or data[p] >> 1 in (32, 33, 34)):
                        raise ValueError('invalid hvc1 sample header or in-band parameter sets')
                    if reject_inband_sps and data[p] & 31 in (7, 13, 15):
                        raise ValueError('in-band SPS unsupported for crop repair')
                    p+=n
        result['tracks'].append(item)
    result['mdat_sha256']=[sha(x.data) for x in mdats]
    return result

def direct_child(boxes, typ):
    matches = [box for box in boxes if box.typ == typ]
    if len(matches) != 1:
        raise ValueError(f'expected one direct {typ!r}, found {len(matches)}')
    return matches[0]


def donor_ncdt(blob):
    """Read the direct Nikon metadata atom from a complete donor MOV."""
    roots = parse(blob)
    ftyp = direct_child(roots, b'ftyp')
    if ftyp.start != 0 or ftyp.header != 8 or ftyp.data[12:16] != b'niko':
        raise ValueError('donor lacks niko at file offset 20 in ftyp')
    moov = direct_child(roots, b'moov')
    udta = direct_child(moov.children, b'udta')
    ncdt = direct_child(parse(udta.data), b'NCDT')
    children = parse(ncdt.data)
    if len(children) > 16:
        raise ValueError('more than 16 NCDT children')
    header = direct_child(children, b'NCHD').data
    if (len(header) != 10 or header[:6] != b'Nikon\0'
            or header[6] not in (1, 2) or header[8:] != b'\0\0'):
        raise ValueError('unsupported NCHD header')
    if not direct_child(children, b'NCTG').data:
        raise ValueError('empty NCTG in donor')
    for typ, max_size in [(b'NCTH', 65536), (b'NCVW', 0x300000)]:
        payload = direct_child(children, typ).data
        if len(payload) > max_size or not payload.startswith(b'\xff\xd8'):
            raise ValueError(f'expected a bounded JPEG in donor {typ!r}')
    db = direct_child(children, b'NCDB')
    if len(parse(db.data)) > 16:
        raise ValueError('more than 16 NCDB children')
    return ncdt.encode()


def add_ncdt(moov, encoded):
    blocks = [box for box in moov.children if box.typ == b'udta']
    if not blocks:
        moov.children.append(Box(b'udta', encoded))
        return
    udta = direct_child(blocks, b'udta')
    existing = [box.encode() for box in parse(udta.data) if box.typ == b'NCDT']
    if existing == [encoded]:
        return
    raise ValueError('existing udta differs from donor or has no NCDT; refusing to alter private metadata')


def rewrite(blob, niko=False, remove_mebx=False, audio_stco=False, size_from_sps=False,
            ncdt_donor=None, crop_to_container=False):
    if size_from_sps and crop_to_container:
        raise ValueError('choose either SPS display to container or container to SPS crop')
    roots=parse(blob)
    if any(x.typ not in (b'ftyp', b'moov', b'mdat', b'free', b'wide', b'skip') for x in roots):
        raise ValueError('unsupported top-level box in Live writer input')
    moov=direct_child(roots, b'moov')
    if any(moov.all(typ) for typ in (b'mvex', b'saio', b'iloc')):
        raise ValueError('fragmented/auxiliary/item offsets in Live input')
    for meta in moov.all(b'meta'):
        skip = 4 if meta.data.startswith(b'\0\0\0\0') else 0
        if any(atom.typ not in (b'hdlr', b'keys', b'ilst') for atom in parse(meta.data, skip)):
            raise ValueError('unsupported item addressing in moov metadata')
    local_reference = struct.pack('>III4sI', 0, 1, 12, b'url ', 1)
    for track in moov.all(b'trak'):
        dref = track.one(b'dref').data
        # Live writer uses one local URL and all sample entries point to it.
        if dref != local_reference:
            raise ValueError('unsupported external or non-writer data reference')
        descriptions = track.one(b'stsd').children
        if len(descriptions) != 1 or descriptions[0].data[6:8] != b'\0\1':
            raise ValueError('unsupported sample data-reference index')
    if ncdt_donor is not None:
        add_ncdt(moov, donor_ncdt(ncdt_donor))
    if niko:
        f=direct_child(roots, b'ftyp')
        if f.start!=0 or f.header!=8 or len(f.data)<12:raise ValueError('requires standard ftyp at zero')
        # 固定第二 compatible brand；保留其余 brands。
        f.data=f.data[:12]+b'niko'+(f.data[16:] if len(f.data)>=16 and f.data[12:16]==b'niko' else f.data[12:])
    if remove_mebx:
        for t in list(moov.children):
            if t.typ==b'trak' and handler(t)==b'meta':
                sd=t.one(b'stsd')
                if any(x.typ!=b'mebx' for x in sd.children):raise ValueError('unknown meta track')
                moov.children.remove(t)
    for t in moov.all(b'trak'):
        if audio_stco and handler(t)==b'soun':
            co,offs=offsets(t)
            if any(v>0xffffffff for v in offs):raise ValueError('32-bit offset overflow')
            co.typ=b'stco';co.data=co.data[:8]+b''.join(pack(v) for v in offs)
        if crop_to_container and handler(t) == b'vide':
            if t.all(b'hvc1'):
                raise ValueError('--crop-to-container supports AVC. HEVC uses the SPS conformance window.')
            width, height = struct.unpack_from('>HH', t.one(b'avc1').prefix, 24)
            tkhd_size = struct.unpack('>II', t.one(b'tkhd').data[-8:])
            if tkhd_size != (width << 16, height << 16):
                raise ValueError('avc1 and tkhd geometry disagree')
            avcc = t.one(b'avcC')
            avcc.data = crop_avcc(avcc.data, width, height)
        if size_from_sps and handler(t)==b'vide':
            a, geometry, _, _ = video_description(t)
            w, h = geometry['display']
            if max(w, h) > 65535:
                raise ValueError('SPS display dimensions exceed sample entry')
            p=bytearray(a.prefix);struct.pack_into('>HH',p,24,w,h);a.prefix=bytes(p)
            tk=t.one(b'tkhd');p=bytearray(tk.data);oldw=u32(p,len(p)-8);oldh=u32(p,len(p)-4)
            # 标准 90/180 度矩阵平移随尺寸一起更新，避免仅改尺寸后裁切。
            matrix=len(p)-44; vals=list(struct.unpack_from('>9I',p,matrix))
            if vals[0]==0 and vals[1]==65536 and vals[3]==0xffff0000 and vals[4]==0:vals[6]=h<<16
            elif vals[0]==0 and vals[1]==0xffff0000 and vals[3]==65536 and vals[4]==0:vals[7]=w<<16
            elif vals[0]==0xffff0000 and vals[4]==0xffff0000:vals[6]=w<<16;vals[7]=h<<16
            struct.pack_into('>9I',p,matrix,*vals);struct.pack_into('>II',p,len(p)-8,w<<16,h<<16);tk.data=bytes(p)
    mappings=[];newpos=0
    for x in roots:
        if x.typ==b'mdat':mappings.append((x.start+x.header,x.start+len(x.encode()),newpos+x.header))
        newpos+=len(x.encode())
    for t in moov.all(b'trak'):
        co,offs=offsets(t);new=[]
        for v in offs:
            candidates=[dest+v-lo for lo,hi,dest in mappings if lo<=v<hi]
            if len(candidates)!=1:raise ValueError('external/invalid chunk offset')
            new.append(candidates[0])
        step=4 if co.typ==b'stco' else 8
        if step == 4 and any(offset > 0xffffffff for offset in new):
            raise ValueError('relocated chunk offset exceeds 32 bits')
        co.data=co.data[:8]+b''.join(v.to_bytes(step,'big') for v in new)
    out=b''.join(x.encode() for x in roots)
    before=inspect(blob, reject_inband_sps=crop_to_container);after=inspect(out)
    if before['mdat_sha256'] != after['mdat_sha256']:
        raise ValueError('mdat payload changed')
    expected=[t['sample_sha256'] for t in before['tracks'] if not(remove_mebx and t['handler']=='meta')]
    if expected != [t['sample_sha256'] for t in after['tracks']]:
        raise ValueError('sample data changed')
    return out


def write_new(path, payload):
    """Publish one complete result atomically without replacing an existing name."""
    if not path.resolve().is_relative_to(Path(__file__).resolve().parent):
        raise ValueError('output must be inside tool folder')
    with tempfile.NamedTemporaryFile(dir=path.parent) as tmp:
        tmp.write(payload)
        tmp.flush()
        os.link(tmp.name, path)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('input',type=Path);p.add_argument('output',type=Path,nargs='?')
    for flag in ('niko','remove-mebx','audio-stco','size-from-sps'):p.add_argument('--'+flag,action='store_true')
    p.add_argument('--ncdt-from', type=Path, metavar='DONOR.MOV',
                   help='仅移植 donor 内的 moov/udta/NCDT；原样保留 donor 内的图像和描述')
    p.add_argument('--crop-to-container', action='store_true',
                   help='按容器尺寸裁去渐进 4:2:0 SPS 末尾宏块的补齐行')
    p.add_argument('--all',action='store_true',help='启用四项原有修正；NCDT 须显式指定 donor')
    p.add_argument('--inspect',action='store_true');a=p.parse_args();blob=a.input.read_bytes()
    if a.inspect:print(json.dumps(inspect(blob),indent=2));return
    if not a.output:p.error('output required')
    root=Path(__file__).resolve().parent
    if not a.output.resolve().is_relative_to(root):p.error('output must be inside tool folder')
    if a.output.exists():p.error('refusing to overwrite existing output')
    try:
        out=rewrite(blob, ncdt_donor=a.ncdt_from.read_bytes() if a.ncdt_from else None,
                    crop_to_container=a.crop_to_container,
                    **{k:a.all or getattr(a,k) for k in ('niko','remove_mebx','audio_stco','size_from_sps')})
    except ValueError as error:
        p.error(str(error))
    try:
        write_new(a.output, out)
    except FileExistsError:
        p.error('refusing to overwrite existing output')
    print(json.dumps(inspect(out),indent=2))
if __name__=='__main__':main()
