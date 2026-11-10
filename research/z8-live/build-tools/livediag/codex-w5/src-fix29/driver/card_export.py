# SPDX-License-Identifier: GPL-2.0-or-later
"""Bounded, read-only catalog of an extracted exFAT virtual-card volume.

Reports names/sizes only. A file entry is not proof of a decoded photograph.
Layout reference: Microsoft exFAT specification, sections3 and7.
"""
import argparse
import json
import math
from pathlib import Path
import struct


def catalog(path):
    path=Path(path)
    if not path.is_file():raise ValueError('regular extracted volume file required')
    size=path.stat().st_size;rows=[];visited=set();base=volume_offset(path);size-=base
    with path.open('rb') as stream:
        def read(offset,length):
            if offset<0 or length<0 or offset+length>size or length>16*1024*1024:
                raise ValueError('out-of-range volume read')
            stream.seek(base+offset);data=stream.read(length)
            if len(data)!=length:raise ValueError('short volume read')
            return data
        boot=read(0,512)
        if boot[3:11]!=b'EXFAT   ' or not 9<=boot[108]<=12 or boot[109]>16:
            raise ValueError('unsupported exFAT geometry')
        sector=1<<boot[108];cluster=sector<<boot[109]
        fat,fat_length,heap,count,root=struct.unpack_from('<IIIII',boot,80)
        if heap*sector+count*cluster>size or count*4+8>fat_length*sector:
            raise ValueError('inconsistent volume geometry')
        def clusters(first,length=None,contiguous=False):
            current=first;seen=set();remaining=math.ceil(length/cluster) if length is not None else None
            while current<0xfffffff8 and remaining!=0:
                if not 2<=current<count+2 or current in seen or len(seen)>=128:
                    raise ValueError('invalid or excessive directory cluster chain')
                seen.add(current);yield current
                if remaining is not None:remaining-=1
                current=current+1 if contiguous else struct.unpack('<I',read(fat*sector+current*4,4))[0]
        def directory(first,prefix='',length=None,contiguous=False,depth=0):
            if first in visited or depth>6 or len(rows)>1024:raise ValueError('directory traversal bound reached')
            visited.add(first)
            chunks=[];total=0
            for c in clusters(first,length,contiguous):
                total+=cluster
                if total>16*1024*1024:raise ValueError('directory byte bound reached')
                chunks.append(read(heap*sector+(c-2)*cluster,cluster))
            data=b''.join(chunks)
            i=0
            while i+32<=len(data):
                kind=data[i]
                if kind==0:return
                if kind!=0x85:i+=32;continue
                secondary=data[i+1];end=i+32*(secondary+1)
                if not 2<=secondary<=18 or end>len(data):raise ValueError('invalid file entry set')
                group=data[i:end];checksum=0
                for n,value in enumerate(group):
                    if n not in (2,3):checksum=(((checksum>>1)|((checksum&1)<<15))+value)&0xffff
                if checksum!=struct.unpack_from('<H',group,2)[0] or group[32]!=0xc0:
                    raise ValueError('file entry checksum or stream invalid')
                names=[group[o+2:o+32] for o in range(64,len(group),32) if group[o]==0xc1]
                name=b''.join(names)[:group[35]*2].decode('utf-16le')
                if len(name)!=group[35] or '/' in name or name in ('.','..'):
                    raise ValueError('unexpected directory name')
                attributes=struct.unpack_from('<H',group,4)[0]
                valid=struct.unpack_from('<Q',group,40)[0]
                start=struct.unpack_from('<I',group,52)[0]
                length=struct.unpack_from('<Q',group,56)[0]
                no_fat_chain=bool(group[33]&2)
                row=dict(path=prefix+name,directory=bool(attributes&0x10),bytes=length,
                    valid_bytes=valid,first_cluster=start,contiguous=no_fat_chain)
                if len(rows)>=1024:raise ValueError('directory entry bound reached')
                rows.append(row)
                if row['directory']:directory(start,prefix+name+'/',length,no_fat_chain,depth+1)
                i=end
        directory(root)
    return dict(volume=str(path),files=rows,capture_verified=False)


if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('volume',type=Path)
    args=ap.parse_args();print(json.dumps(catalog(args.volume),indent=2))

def volume_offset(path):
    with Path(path).open('rb') as stream:
        first=stream.read(512)
    if first[3:11]==b'EXFAT   ':return 0
    if first[510:512]!=b'\x55\xaa':raise ValueError('No MBR or exFAT signature')
    return struct.unpack_from('<I',first,0x1c6)[0]*512

# SPDX-License-Identifier: GPL-2.0-or-later
"""Extract one actual file from an already audited, stopped virtual card.

This only reads the volume. It never constructs a camera file or changes a
card. The existing directory parser validates entry checksums and geometry.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct



def extract(volume, name, output):
    volume, output = Path(volume), Path(output)
    rows = [x for x in catalog(volume)['files'] if x['path'] == name and not x['directory']]
    if len(rows) != 1:
        raise ValueError('One exact, validated file entry required')
    row = rows[0]
    length = row['bytes']
    if not 0 < length <= 256*1024*1024 or row['valid_bytes'] != length:
        raise ValueError('Bounded, fully initialized file required')
    if output.exists() or not output.parent.is_dir():
        raise ValueError('Fresh output in an existing private directory required')
    digest = hashlib.sha256()
    with volume.open('rb') as src, output.open('xb') as dst:
        base=volume_offset(volume);src.seek(base);boot = src.read(512)
        sector = 1 << boot[108]
        cluster = sector << boot[109]
        fat, _, heap, count, _ = struct.unpack_from('<IIIII', boot, 80)
        current = row['first_cluster']
        seen = set()
        left = length
        for _ in range(math.ceil(length/cluster)):
            if not 2 <= current < count+2 or current in seen:
                raise ValueError('Invalid, short or cyclic actual file chain')
            seen.add(current)
            offset = base+heap*sector+(current-2)*cluster
            size = min(left, cluster)
            if offset+size > volume.stat().st_size:
                raise ValueError('Actual file span exceeds the volume')
            src.seek(offset)
            data = src.read(size)
            if len(data) != size:
                raise ValueError('Short actual file read')
            dst.write(data)
            digest.update(data)
            left -= size
            if left:
                if row['contiguous']:
                    current += 1
                else:
                    src.seek(base+fat*sector+current*4)
                    value = src.read(4)
                    if len(value) != 4:
                        raise ValueError('Short FAT read')
                    current = struct.unpack('<I', value)[0]
        if left:
            raise ValueError('Actual file incomplete')
    result = dict(volume=str(volume), entry=row, output=str(output), bytes=length,
                  sha256=digest.hexdigest(), camera_image_decoded=False,
                  scope='actual initialized file bytes extracted read-only; no substituted data, codec or camera-file acceptance')
    with output.with_suffix(output.suffix+'.extraction.json').open('x', encoding='utf-8') as receipt:
        receipt.write(json.dumps(result, indent=2)+'\n')
    return result


