#!/usr/bin/env python3
"""Explicit synthetic resource4e, boot ROM input only; not factory tone2.
Reuse exact bundled tone1 PCM as a known 4800-byte sample, not a guessed asset.
"""
from pathlib import Path
import hashlib,json,struct
P=Path(__file__).resolve().parent
# Exact official MAIN identity guards the reused bytes.
MAIN=Path('/path/to/Nikon/_mac-migration/firmware/z8-c311-main.bin')
d=MAIN.read_bytes();sha=lambda b:hashlib.sha256(b).hexdigest()
assert sha(d)=='2b43a31c9de838f84b12416de4348a7004e65be07f853ec56ec2b10f3a8e3853'
a=0x43fba590;pcm=d[a-0x40160000:a-0x40160000+4800];assert len(pcm)==4800 and any(pcm)
blob=bytearray(0xee00);struct.pack_into('<IH',blob,0x1c,0x40,len(pcm));blob[0x40:0x40+len(pcm)]=pcm
name='resource4e-tone2-synthetic.bin';(P/name).write_bytes(blob)
r=dict(schema='z8-explicit-sfx-resource-input/v1',synthetic=True,factory_resource=False,hardware_sound_verified=False,loader=dict(file=name,address='0x46460000',bytes=len(blob),sha256=sha(blob),force_raw=True),resource_id='0x4e',bank_index=24,bank_extent=0x20000,declared_resource_bytes=0xee00,source_main=dict(path=str(MAIN),sha256=sha(d)),pcm=dict(provenance='Exact bundled native tone1 bytes reused only as synthetic tone2 input',source_address=hex(a),source_offset=hex(a-0x40160000),bytes=len(pcm),sha256=sha(pcm)),header=dict(tone2_pointer_offset_field='0x1c (u32 LE)',tone2_pointer_offset='0x40',tone2_bytes_field='0x20 (u16 LE)',tone2_bytes=len(pcm),other_resource_tones='left zero; normal tone1/2 test only'),invariants=['No dynamic guest RAM writes','No firmware or timeout changes','No fake completion/IRQ','Only explicit startup ROM loader supplies missing resource bytes','No recovered camera tone2 or real speaker acoustic claim'],native_evidence=['40e34194=resource4e getter call; 40e341b4 offset header+1c; 40e341d8 length header+20; 40e34218 publishes u16 a038feb10','40e3469c resource2 returns a038feb08; 40e348f4 resource2 returns u16 a038feb10','40e34314..33c copies built-in tone1 PCM from43fba590 to a15c19f00, exactly4800 bytes'],qemu_argument=f'-device loader,file={name},addr=0x46460000,force-raw=on')
(P/'manifest.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps({'bytes':len(blob),'sha256':sha(blob),'pcm_sha256':sha(pcm)}))
