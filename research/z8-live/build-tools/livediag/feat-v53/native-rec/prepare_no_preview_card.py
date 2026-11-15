"""QEMU fixture: unmodified captured HEVC/media atoms, remove only trailing Live preview free atom."""
from pathlib import Path
import hashlib,json,struct,sys,subprocess
sys.path.insert(0,'/path/to/user/fxfix/v52-root/driver-g/retry-driver')
import card_export,exfat_put
r=Path('/path/to/user/fxfix/v53-native-rec/probe-n2')
source=Path('/path/to/user/.local/share/nikon-z8-qemu/live-audio/regress/runs/v53a-core-rec/files/LIVE/100NCZ_8/DSC_0001.MOV')
b=source.read_bytes();assert hashlib.sha256(b).hexdigest()=='9bfc31181c1c0ea8e4f53bfe75687a47a0bdfe8736397db5f800719ed1695ef4'
at=0;atoms=[]
while at<len(b):
    n,t=struct.unpack_from('>I4s',b,at);assert n>=8 and at+n<=len(b);atoms.append((at,n,t));at+=n
at,n,t=atoms[-1];assert t==b'free' and b[at+8:at+12]==b'LVJ1' and at+n==len(b)
payload=b[:at];out=r/'captured-no-preview.MOV';out.write_bytes(payload)
image=r/'card3.raw';assert not image.exists();subprocess.run(['cp','--reflink=auto',str(r/'card2.raw'),str(image)],check=True)
catalog=card_export.catalog(image)['files'];folder=next(x for x in catalog if x['path']=='DCIM/100NCZ_8');movie=next(x for x in catalog if x['path']=='DCIM/100NCZ_8/LIV_9012.MOV');cache=next((x for x in catalog if x['path']=='DCIM/100NCZ_8/NC_FLLST.DAT'),None)
assert folder['contiguous'] and movie['contiguous'] and len(payload)<movie['bytes']
with image.open('r+b') as f:
    part=card_export.volume_offset(image);f.seek(part);vbr=f.read(512);_,_,heap,count,root=struct.unpack_from('<IIIII',vbr,80);ss=1<<vbr[108];sc=ss<<vbr[109];co=lambda c:part+heap*ss+(c-2)*sc
    f.seek(co(root));rd=f.read(sc);entry=next(rd[i:i+32] for i in range(0,len(rd),32) if rd[i]==0x81);bm,blen=struct.unpack_from('<IQ',entry,20);f.seek(co(bm));bitmap=bytearray(f.read(blen))
    f.seek(co(folder['first_cluster']));directory=bytearray(f.read(folder['bytes']));i=0;replaced=False
    while directory[i]:
        n=directory[i+1]+1 if directory[i]==0x85 else 1
        if directory[i]==0x85:
            first=struct.unpack_from('<I',directory,i+52)[0]
            if first==movie['first_cluster']:
                entry=exfat_put._entry_set('LIV_9012.MOV',0x20,first,len(payload));assert len(entry)==32*n;directory[i:i+len(entry)]=entry;replaced=True
            if cache and first==cache['first_cluster']:
                assert cache['contiguous']
                for j in range(n):directory[i+32*j]&=0x7f
        i+=32*n
    assert replaced
    for k in range((len(payload)+sc-1)//sc,(movie['bytes']+sc-1)//sc):
        c=movie['first_cluster']+k;bitmap[(c-2)//8]&=~(1<<((c-2)%8))
    if cache:
        for k in range((cache['bytes']+sc-1)//sc):
            c=cache['first_cluster']+k;bitmap[(c-2)//8]&=~(1<<((c-2)%8))
    f.seek(co(movie['first_cluster']));f.write(payload);f.seek(co(folder['first_cluster']));f.write(directory);f.seek(co(bm));f.write(bitmap)
probe=json.loads(subprocess.check_output(['ffprobe','-v','error','-show_streams','-of','json',str(out)]));v=next(x for x in probe['streams'] if x.get('codec_type')=='video');assert v['codec_name']=='hevc' and v['nb_frames']=='100' and v['width']==1024 and v['height']==682
subprocess.run(['ffmpeg','-v','error','-i',str(out),'-map','0:v:0','-f','null','-'],check=True)
(r/'no-preview.json').write_text(json.dumps({'source_sha256':hashlib.sha256(b).hexdigest(),'output_sha256':hashlib.sha256(payload).hexdigest(),'source_bytes':len(b),'output_bytes':len(payload),'removed_preview_bytes':len(b)-len(payload),'media_atoms_byte_identical':True,'frames':100,'size':[1024,682],'audio_tracks':0,'scope':'offline private fixture only, no camera writer change; no preview free atom'},indent=2)+'\n')
