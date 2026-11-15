"""Rename only an existing movie on a private QEMU card; never a mounted card."""
from pathlib import Path
import hashlib,json,struct,sys
sys.path.insert(0,'/path/to/user/fxfix/v52-root/driver-g/retry-driver')
import card_export,exfat_put
image=Path('/path/to/user/fxfix/v53-native-rec/probe-n2/card.raw')
assert image.is_file() and image.stat().st_size==8589934592
catalog=card_export.catalog(image)['files']
folder=next(x for x in catalog if x['path']=='DCIM/100NCZ_8')
movie=next(x for x in catalog if x['path']=='DCIM/100NCZ_8/LIV_9013.MOV')
cache=next((x for x in catalog if x['path']=='DCIM/100NCZ_8/NC_FLLST.DAT'),None)
assert folder['contiguous'] and movie['contiguous']
with image.open('r+b') as f:
    part=card_export.volume_offset(image);f.seek(part);vbr=f.read(512)
    _,_,heap,count,root=struct.unpack_from('<IIIII',vbr,80);ss=1<<vbr[108];sc=ss<<vbr[109]
    co=lambda c:part+heap*ss+(c-2)*sc
    f.seek(co(root));rd=f.read(sc)
    bitmap_entry=next(rd[i:i+32] for i in range(0,len(rd),32) if rd[i]==0x81)
    bm,blen=struct.unpack_from('<IQ',bitmap_entry,20);f.seek(co(bm));bitmap=bytearray(f.read(blen))
    f.seek(co(folder['first_cluster']));directory=bytearray(f.read(folder['bytes']))
    i=0;renamed=False
    while directory[i]:
        n=directory[i+1]+1 if directory[i]==0x85 else 1
        if directory[i]==0x85:
            first=struct.unpack_from('<I',directory,i+52)[0]
            if first==movie['first_cluster']:
                entry=exfat_put._entry_set('LIV_9012.MOV',0x20,first,movie['bytes']);assert len(entry)==32*n
                directory[i:i+len(entry)]=entry;renamed=True
            if cache and first==cache['first_cluster']:
                assert cache['contiguous']
                for j in range(n):directory[i+32*j]&=0x7f
        i+=32*n
    assert renamed
    if cache:
        for k in range((cache['bytes']+sc-1)//sc):
            c=cache['first_cluster']+k;bitmap[(c-2)//8]&=~(1<<((c-2)%8))
    f.seek(co(folder['first_cluster']));f.write(directory)
    f.seek(co(bm));f.write(bitmap)
after=card_export.catalog(image);assert any(x['path']=='DCIM/100NCZ_8/LIV_9012.MOV' for x in after['files'])
assert any(x['path']=='DCIM/100NCZ_8/LIV_9013.JPG' for x in after['files'])
assert not any(x['path'].endswith('NC_FLLST.DAT') for x in after['files'])
after['scope']='Private synthetic pair: original JPG9013 plus older MOV9012; main media unchanged, cache rebuild on boot'
image.with_suffix('.json').write_text(json.dumps(after,indent=2)+'\n')
