"""Pack the H.265 Live build (b12501ea with the v37 Live/playback layers rebuilt for HEVC) into Z_8_0406.bin. Runs on win-lab.
Usage: pack45.py HEVC_SHA256 QEMU_RESULT_JSON"""
from pathlib import Path
import json,binascii,hashlib,sys,tempfile,shutil,zipfile
T=Path.home()/'fxfix/pack';P=Path.home()/'fxfix/portrait';G=Path.home()/'fxfix/c64gate';N=Path.home()/'Nikon'
sys.path.insert(0,str(N/'NikonFirmwarePatch'))
from z8prores.package import Package
from verify import decode
sha=lambda b:hashlib.sha256(b).hexdigest()
H=Path.home()/'fxfix/hevc'
main=(H/'q/hevc.main').read_bytes();PIN=sys.argv[1]
info=json.loads((H/'hevc.main.json').read_text())
raw=(N/'z8-qemu/.local/firmware/Z_8_0311.bin').read_bytes()
assert sha(main)==PIN==info['sha256'] and sha(raw)=='d67413c2ede6b9ce8887175f20700bd9fbad2a973a4820968c45503d15d80d96'
assert info['base']=='b12501ea'
assert main[0x14eaa90:0x14eaa98]==main[-9:-1]==b'04.06.p0'
wlm=(P/'tree/build/portrait/portrait.wlm').read_bytes()
packet,_=Package(raw).replace_eg(main+binascii.crc_hqx(main,0).to_bytes(2,'big'),'eg2070_0406p0.bi')
name='Z8-C64Switch-LivePhoto-W5share-Flat-PoolFix-NFP040-LiveV37-H265-C64Label-PortraitPlugin-BootFix2-USB-'+PIN[:8];out=T/name
assert not out.exists()
with tempfile.TemporaryDirectory(dir=T) as tmp:
    d=Path(tmp)/'delivery';d.mkdir()
    (d/'Z_8_0406.bin').write_bytes(packet)
    a,b=decode(raw),decode(packet)
    assert a[:112]==b[:112] and a[128:581844]==b[128:581844] and a[82389555:-16]==b[82389555:-16]
    assert b[112:128]==b'eg2070_0406p0.bi' and b[581844:82389553]==main
    shutil.copytree(N/'_bundle-20261102/bridge64/card',d/'FENESTRA')
    (d/'FENESTRA/portrait.wlm').write_bytes(wlm)
    (d/'verification.json').write_text(json.dumps(dict(sha256=sha(packet),bytes=len(packet),source=sha(raw),main=PIN,identity='04.06.p0',
        eg=b[112:128].decode(),base_main='b12501ea08c28848',
        layers=['v37 191e6e4f rebuilt from stock C3.11 (fenestra-z8 codex/z8-live-v37 fcb0e1c)','C64 i-menu label font x275 (as v36b)',
                'portrait video gated by RAM word 0xa116d3480; slots.state ops 13/14','portrait movie brightness display: OSD table B := A, 0x1f41 portrait rect (252,368,414,450)',
                'C64 switch: slot limit word 0xa116d3400 boot init 0x40 -> 9 (C10-C64 off = stock C1-C9); slots.state SLOTS_INFO raises it to 64 when slotcard.wlm starts',
                'Live H.265: Wave5 codec word 1 (HEVC Main 8-bit), IRAP random access, SPS display crop, hvc1/hvcC MOV; v37 Live+playback layers rebuilt (v37 hevc '+info['hevc_v37'][:8]+') and spliced; bootguard build id restamped'],
        hevc=info,portrait_wlm=dict(sha256=sha(wlm),bytes=len(wlm)),body_crc=True,header_digest=True,
        host='hevc_test: QEMU x265 sample + VideoToolbox 1024x688 sample, mid-stream IRAP start, SPS crop pixel-identical',qemu=json.loads(Path(sys.argv[2]).read_text()),physical_camera_verified=False),indent=2)+'\n')
    (d/'README.txt').write_text((H/'README45.txt').read_text(encoding='utf-8'),encoding='utf-8')
    (d/'SHA256SUMS.txt').write_text(''.join(f'{sha(f.read_bytes())}  {f.relative_to(d).as_posix()}\n' for f in sorted(d.rglob('*')) if f.is_file()))
    d.rename(out)
z=T/(name+'.zip')
with zipfile.ZipFile(z,'x',zipfile.ZIP_DEFLATED) as zf:
    for f in sorted(out.rglob('*')):
        if f.is_file():zf.write(f,f.relative_to(T).as_posix())
print(out,sha(packet),z,z.stat().st_size)
