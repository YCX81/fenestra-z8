#!/usr/bin/env python3
"""Pack the exact QEMU-tested v51d MAIN; no camera or mounted-card I/O.
Run in win-lab WSL: pack51.py /path/to/final-verification.json
"""
from pathlib import Path
import sys,hashlib,json,binascii,tempfile,shutil,zipfile
H=Path('/path/to/user');D=H/'fxfix/v51d';T=H/'fxfix/pack';N=H/'Nikon'
sys.path.insert(0,str(N/'NikonFirmwarePatch'))
from z8prores.package import Package
from verify import decode
sha=lambda b:hashlib.sha256(b).hexdigest()
PIN='aa6e94e0042fb445c666de73c0f6b9e7e569010b7993a3ac3da0979b174a9433'
info=json.loads((D/'v51.main.json').read_text());verification=json.loads(Path(sys.argv[1]).read_text())
main=(D/'q/main.main').read_bytes();assert sha(main)==PIN==info['sha256']==verification['main_sha256']
assert verification['writer_host_passed'] is True
assert verification['binary_audit_passed'] is True
assert verification['release_status'] == 'offline_test_candidate'
assert verification['physical_camera_verified'] is False
raw=(N/'z8-qemu/.local/firmware/Z_8_0311.bin').read_bytes()
assert sha(raw)=='d67413c2ede6b9ce8887175f20700bd9fbad2a973a4820968c45503d15d80d96'
assert main[0x14eaa90:0x14eaa98]==main[-9:-1]==b'04.06.p0'
packet,_=Package(raw).replace_eg(main+binascii.crc_hqx(main,0).to_bytes(2,'big'),'eg2070_0406p0.bi')
a,b=decode(raw),decode(packet)
assert a[:112]==b[:112] and a[128:581844]==b[128:581844] and a[82389555:-16]==b[82389555:-16]
assert b[112:128]==b'eg2070_0406p0.bi' and b[581844:82389553]==main
assert b[82389553:82389555]==binascii.crc_hqx(main,0).to_bytes(2,'big')
name='Z8-v51-Lab-Portrait-LiveMOV-'+PIN[:8];out=T/name
assert not out.exists() and not out.with_suffix('.zip').exists()
base=next(T.glob('Z8-v49-NfpTextFix-*63240425'))
with tempfile.TemporaryDirectory(dir=T) as tmp:
 p=Path(tmp)/'package';p.mkdir()
 (p/'Z_8_0406.bin').write_bytes(packet)
 shutil.copytree(base/'FENESTRA',p/'FENESTRA')
 verification.update(version='v51',identity='04.06.p0',package_sha256=sha(packet),package_bytes=len(packet),stock_package_sha256=sha(raw),aggregate=info,body_crc_verified=True,container_unchanged_outside_main=True)
 (p/'verification.json').write_text(json.dumps(verification,ensure_ascii=False,indent=2)+'\n')
 shutil.copyfile(D/'README51.txt',p/'README.txt')
 (p/'SHA256SUMS.txt').write_text(''.join(f'{sha(f.read_bytes())}  {f.relative_to(p).as_posix()}\n' for f in sorted(p.rglob('*')) if f.is_file()))
 p.rename(out)
z=T/(name+'.zip')
with zipfile.ZipFile(z,'x',zipfile.ZIP_DEFLATED) as f:
 for p in sorted(out.rglob('*')):
  if p.is_file():f.write(p,p.relative_to(T).as_posix())
with zipfile.ZipFile(z) as f:assert f.testzip() is None
print(json.dumps(dict(directory=str(out),zip=str(z),package_sha256=sha(packet),main_sha256=PIN),indent=2))
