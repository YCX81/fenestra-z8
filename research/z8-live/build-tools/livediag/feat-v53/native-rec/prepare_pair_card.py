"""Offline DCF same-number experiment, only on a fresh private virtual-card copy."""
from pathlib import Path
import hashlib,json,struct,sys
sys.path.insert(0,'/path/to/user/fxfix/v52-root/driver-g/retry-driver')
import card_export,exfat_put

image=Path('/path/to/user/fxfix/v53-native-rec/pair-card.raw')
source=Path('/path/to/user/.local/share/nikon-z8-qemu/live-audio/regress/runs/v52g-final-rec/files/DCIM/100NCZ_8/DSC_0001.JPG')
assert image.is_file() and image.stat().st_size==8589934592
catalog=card_export.catalog(image)['files']
folder=next(x for x in catalog if x['path']=='DCIM/100NCZ_8')
assert folder['contiguous'] and folder['directory']
assert not any(x['path'].endswith('.JPG') for x in catalog)
assert any(x['path']=='DCIM/100NCZ_8/LIV_9013.MOV' for x in catalog)
payload=source.read_bytes()
with image.open('r+b') as f:
    part=card_export.volume_offset(image);f.seek(part);vbr=f.read(512)
    fat,_,heap,count,root=struct.unpack_from('<IIIII',vbr,80)
    ss=1<<vbr[108];sc=ss<<vbr[109]
    co=lambda c:part+heap*ss+(c-2)*sc
    f.seek(co(root));rd=f.read(sc)
    bitmap_entry=next(rd[i:i+32] for i in range(0,len(rd),32) if rd[i]==0x81)
    bm,blen=struct.unpack_from('<IQ',bitmap_entry,20)
    f.seek(co(bm));bitmap=bytearray(f.read(blen))
    f.seek(co(folder['first_cluster']));directory=bytearray(f.read(folder['bytes']))
    # The file-list cache belongs to this private fixture. Retire its entry and
    # allocation so boot really rescans the added same-basename JPG.
    cache=next(x for x in catalog if x['path']=='DCIM/100NCZ_8/NC_FLLST.DAT')
    assert cache['contiguous'] and not cache['directory']
    i=0
    while directory[i]:
        n=directory[i+1]+1 if directory[i]==0x85 else 1
        if directory[i]==0x85 and struct.unpack_from('<I',directory,i+52)[0]==cache['first_cluster']:
            for j in range(n):directory[i+32*j]&=0x7f
        i+=32*n
    for k in range((cache['bytes']+sc-1)//sc):
        c=cache['first_cluster']+k;bitmap[(c-2)//8]&=~(1<<((c-2)%8))
    n=(len(payload)+sc-1)//sc
    cluster=next(c for c in range(2,count+2-n) if all(not bitmap[(c+k-2)//8]&(1<<((c+k-2)%8)) for k in range(n)))
    for k in range(n):bitmap[(cluster+k-2)//8]|=1<<((cluster+k-2)%8)
    entry=exfat_put._entry_set('LIV_9013.JPG',0x20,cluster,len(payload))
    assert i+len(entry)+32<=len(directory)
    directory[i:i+len(entry)]=entry
    f.seek(co(cluster));f.write(payload)
    f.seek(co(folder['first_cluster']));f.write(directory)
    f.seek(co(bm));f.write(bitmap)
after=card_export.catalog(image)
assert any(x['path']=='DCIM/100NCZ_8/LIV_9013.JPG' for x in after['files'])
assert not any(x['path'].endswith('NC_FLLST.DAT') for x in after['files'])
after.update(experiment='one real guest JPG plus known native-playable MOV, same DCF basename; no firmware changes',
    added_jpeg_sha256=hashlib.sha256(payload).hexdigest(),source_jpeg=str(source))
image.with_suffix('.json').write_text(json.dumps(after,indent=2)+'\n')
print(after['experiment'])
