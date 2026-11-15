"""Package an explicitly verified v52 test candidate; regular files only."""
from pathlib import Path
import argparse,binascii,hashlib,json,shutil,sys,tempfile,zipfile

p=argparse.ArgumentParser();p.add_argument('verification',type=Path);p.add_argument('readme',type=Path);p.add_argument('output',type=Path)
a=p.parse_args();v=json.loads(a.verification.read_text());sha=lambda b:hashlib.sha256(b).hexdigest()
assert v['release_status']=='qemu_test_candidate' and v['core_regression_passed'] is True
assert v['binary_audit_passed'] is True and v['physical_camera_verified'] is False
main=Path(v['main_path']).read_bytes();assert sha(main)==v['main_sha256']
H=Path('/path/to/user');N=H/'Nikon';sys.path.insert(0,str(N/'NikonFirmwarePatch'))
from z8prores.package import Package
from verify import decode
raw=(N/'z8-qemu/.local/firmware/Z_8_0311.bin').read_bytes()
assert sha(raw)=='d67413c2ede6b9ce8887175f20700bd9fbad2a973a4820968c45503d15d80d96'
assert main[0x14eaa90:0x14eaa98]==main[-9:-1]==b'04.06.p0'
packet,_=Package(raw).replace_eg(main+binascii.crc_hqx(main,0).to_bytes(2,'big'),'eg2070_0406p0.bi')
assert sha(packet)==v['expected_package_sha256'], 'independent package audit differs'
old,new=decode(raw),decode(packet)
assert old[:112]==new[:112] and old[128:581844]==new[128:581844] and old[82389555:-16]==new[82389555:-16]
assert new[112:128]==b'eg2070_0406p0.bi' and new[581844:82389553]==main
assert new[82389553:82389555]==binascii.crc_hqx(main,0).to_bytes(2,'big')
name='Z8-v52-Live-EVF-'+v['main_sha256'][:8];a.output.mkdir(parents=True,exist_ok=True)
out=a.output/name;z=a.output/(name+'.zip');assert not out.exists() and not z.exists()
base=H/'fxfix/pack/Z8-v51-Portrait-LiveMOV-aa6e94e0'
assert base.is_dir()
with tempfile.TemporaryDirectory(dir=a.output) as tmp:
    d=Path(tmp)/name;d.mkdir();(d/'Z_8_0406.bin').write_bytes(packet)
    shutil.copytree(base/'FENESTRA',d/'FENESTRA')
    for f in (d/'FENESTRA').rglob('*'):
        if f.is_file():assert f.read_bytes()==(base/f.relative_to(d)).read_bytes()
    for item in v.get('test_movies',[]):
        data=Path(item['source']).read_bytes();assert sha(data)==item['sha256']
        target=d/'TEST_MOVIES'/item['name'];assert target.parent==d/'TEST_MOVIES'
        target.parent.mkdir(exist_ok=True);target.write_bytes(data)
    v.update(version='v52',identity='04.06.p0',package_sha256=sha(packet),package_bytes=len(packet),
             stock_package_sha256=sha(raw),body_crc_verified=True,container_unchanged_outside_main=True)
    (d/'verification.json').write_text(json.dumps(v,ensure_ascii=False,indent=2)+'\n')
    shutil.copyfile(a.readme,d/'README.txt')
    (d/'SHA256SUMS.txt').write_text(''.join(f'{sha(f.read_bytes())}  {f.relative_to(d).as_posix()}\n' for f in sorted(d.rglob('*')) if f.is_file()))
    d.rename(out)
with zipfile.ZipFile(z,'x',zipfile.ZIP_DEFLATED) as archive:
    for f in sorted(out.rglob('*')):
        if f.is_file():archive.write(f,f.relative_to(a.output).as_posix())
with zipfile.ZipFile(z) as archive:assert archive.testzip() is None
print(json.dumps(dict(directory=str(out),zip=str(z),zip_sha256=sha(z.read_bytes()),package_sha256=sha(packet),main_sha256=v['main_sha256']),indent=2))
