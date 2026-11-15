"""Package the one-MOV candidate (v54) from a composed MAIN. No camera/card operations."""
from pathlib import Path
import binascii,hashlib,json,shutil,sys,zipfile
main_path,verification,readme,output=map(Path,sys.argv[1:5])
sha=lambda b:hashlib.sha256(b).hexdigest()
v=json.loads(verification.read_text());main=main_path.read_bytes();assert sha(main)==v['main_sha256']
H=Path('/path/to/user');N=H/'Nikon';sys.path.insert(0,str(N/'NikonFirmwarePatch'))
from z8prores.package import Package
from verify import decode
raw=(N/'z8-qemu/.local/firmware/Z_8_0311.bin').read_bytes()
assert sha(raw)=='d67413c2ede6b9ce8887175f20700bd9fbad2a973a4820968c45503d15d80d96'
assert main[0x14eaa90:0x14eaa98]==main[-9:-1]==b'04.06.p0'
packet,_=Package(raw).replace_eg(main+binascii.crc_hqx(main,0).to_bytes(2,'big'),'eg2070_0406p0.bi')
old,new=decode(raw),decode(packet)
assert old[:112]==new[:112] and old[128:581844]==new[128:581844] and old[82389555:-16]==new[82389555:-16]
assert new[112:128]==b'eg2070_0406p0.bi' and new[581844:82389553]==main
assert new[82389553:82389555]==binascii.crc_hqx(main,0).to_bytes(2,'big')
name='Z8-v56-Live-OneMov-'+v['main_sha256'][:8];out=output/name;z=output/(name+'.zip')
assert not out.exists() and not z.exists();out.mkdir(parents=True)
(out/'Z_8_0406.bin').write_bytes(packet)
shutil.copytree(H/'fxfix/pack/Z8-v51-Portrait-LiveMOV-aa6e94e0/FENESTRA',out/'FENESTRA')
v.update(version='v56',identity='04.06.p0',package_sha256=sha(packet),package_bytes=len(packet),physical_camera_verified=False)
(out/'verification.json').write_text(json.dumps(v,ensure_ascii=False,indent=2)+'\n')
shutil.copyfile(readme,out/'README.txt')
(out/'SHA256SUMS.txt').write_text(''.join(f'{sha(f.read_bytes())}  {f.relative_to(out).as_posix()}\n' for f in sorted(out.rglob('*')) if f.is_file()))
with zipfile.ZipFile(z,'x',zipfile.ZIP_DEFLATED) as a:
    for f in sorted(out.rglob('*')):
        if f.is_file():a.write(f,f.relative_to(output).as_posix())
print(json.dumps(dict(directory=str(out),zip=str(z),package_sha256=sha(packet),main_sha256=v['main_sha256'])))
