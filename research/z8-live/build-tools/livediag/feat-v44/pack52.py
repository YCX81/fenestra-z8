"""Pack v44 = v43 (cc0849a7) + Live icon overlay re-attach after playback into Z_8_0406.bin. Runs on win-lab.
Usage: pack52.py V44_SHA256 QEMU_RESULT_JSON"""
from pathlib import Path
import json,binascii,hashlib,sys,tempfile,shutil,zipfile
T=Path.home()/'fxfix/pack';P=Path.home()/'fxfix/portrait';G=Path.home()/'fxfix/c64gate';N=Path.home()/'Nikon'
sys.path.insert(0,str(N/'NikonFirmwarePatch'))
from z8prores.package import Package
from verify import decode
sha=lambda b:hashlib.sha256(b).hexdigest()
H=Path.home()/"fxfix/v44"
main=(H/'q/main.main').read_bytes();PIN=sys.argv[1]
info=json.loads((H/'v44.main.json').read_text())
raw=(N/'z8-qemu/.local/firmware/Z_8_0311.bin').read_bytes()
assert sha(main)==PIN==info['sha256'] and sha(raw)=='d67413c2ede6b9ce8887175f20700bd9fbad2a973a4820968c45503d15d80d96'
assert info['base']=='cc0849a7'
assert main[0x14eaa90:0x14eaa98]==main[-9:-1]==b'04.06.p0'
wlm=(P/'tree/build/portrait/portrait.wlm').read_bytes()
packet,_=Package(raw).replace_eg(main+binascii.crc_hqx(main,0).to_bytes(2,'big'),'eg2070_0406p0.bi')
name='Z8-v44-LiveIconFix-LiveHevcHwFix-SDCacheFix-SDFallback-LiveTwoCard-LiveFontFix-LiveShutterSound-LiveH265-NFP040All4-C64Moved-C64Switch-PortraitPlugin-BootFix2-USB-'+PIN[:8];out=T/name
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
        eg=b[112:128].decode(),base_main='cc0849a774a9',
        layers=['v37 191e6e4f rebuilt from stock C3.11 (fenestra-z8 codex/z8-live-v37 fcb0e1c)','C64 i-menu label font x275 (as v36b)',
                'portrait video gated by RAM word 0xa116d3480; slots.state ops 13/14','portrait movie brightness display: OSD table B := A, 0x1f41 portrait rect (252,368,414,450)',
                'C64 switch: slot limit word 0xa116d3400 boot init 0x40 -> 9 (C10-C64 off = stock C1-C9); slots.state SLOTS_INFO raises it to 64 when slotcard.wlm starts',
                'Live H.265: Wave5 codec word 1 (HEVC Main 8-bit), IRAP random access, SPS display crop, hvc1/hvcC MOV; v37 Live+playback layers rebuilt (v37 hevc 7437d904) and spliced',
                'C64 slot layer moved 0x40B00000 -> 0x40B50000 (boot init 0x40B70000, copy end 0x40B8A000); NFP v0.4.0 log-colour group added (111483 bytes, no collision); bootguard build id restamped',
                'v39 Live shutter sound: Live page row Shutter sound On/Off (text 3163, row 0x7fb3), saved at live-z8.x +28; BL at stock mute-byte read 0x40f9cb64 -> live_shutter_gate mutes only while Live is requested in photo mode; Live+playback layers rebuilt (v37 shutter c437b794) and spliced onto bc39aaba',
                'v40 font: in ja/zh-CN/zh-TW/ko the class-0 CJK face draws ASCII half-width with icons on some codes (q). Live install publishes a copy of the stock class table 0xa02f2ac28 with class 18 = class 0 except those 4 languages use the English Latin entry; Fenestra text-bank ids (>=3154) move from class 0 to 18. Stock ids/tables untouched. Live layer rebuilt (v37 font 5434c24f) and spliced onto 2e48d403',
                'v41 SD fallback: cardslot.c (same bytes as 61d5896f) in RX cave 0x401fe300..0x401fed08; B at stock open/remove/rename/opendir/mkdir wrappers; A:\\FENESTRA routed to B: when A has no FENESTRA and SD has it, choice latched per boot at RAM 0xa1171b000',
                'v41 Live two-card: backup/JPEG+JPEG twin copy of the Live set (live_write_twin, state 0xa1171b100), playback REC lookup keyed by path+size instead of mount epoch, delete/protect sync on A and B, reset on card event 24; merged 3-way onto the v40 Live source and spliced (v37 v41 build 3d7abfe2)',
                'v42 SD cache fix: hw 11-12 showed SD (B:) transfers via stock read 0x402dd6c8 / write 0x402dd78c leave the caller buffer cache-incoherent (stale reads, lost writes) -> .wlm header garbage -> FX_CAPABILITY panic. cardslot cave rebuilt: read hook civac before+after, write hook cvac before, RAM buffers only (0xa00000000..0x1000000000)',
                'v43 Live HEVC on hw: real Wave5 HEADERS gives AUD 3 / VPS 33 / SPS 415 (scaling lists + VUI) / PPS 7 B; Live rejected SPS > 256 B (hevc_open_rc 1) so the encoder never started and no Live was written on any card since v38. live_hevc vps/sps/pps/sps_raw -> 256/1024/1024/1024, SPS_LIMIT 1024, NAL limit = buffer size, live_mov sps/pps 1024, AU headroom 4 KiB. Host test with the captured hw headers: parse+crop ok (conf_win_bottom 3, 688->682), MOV hvcC parser ok',
                'v44 Live icon: hw 11-12 after playback -> shooting the stock UI disabled the global graphics target (1,1,4) that attach had enabled; surface visible, object listed, presents ok but nothing on the LCD. indicator() now queries the stock getter 0x403409d4 (checked against core profile 0x401dee00+192) and re-attaches when the target is off; UiState.target_lost counts it. Manual re-enable on hw kept the target on (no churn)'],
        v44_splice=info,portrait_wlm=dict(sha256=sha(wlm),bytes=len(wlm)),body_crc=True,header_digest=True,
        qemu=json.loads(Path(sys.argv[2]).read_text()),physical_camera_verified=False),indent=2)+'\n')
    (d/'README.txt').write_text((H/'README52.txt').read_text(encoding='utf-8'),encoding='utf-8')
    (d/'SHA256SUMS.txt').write_text(''.join(f'{sha(f.read_bytes())}  {f.relative_to(d).as_posix()}\n' for f in sorted(d.rglob('*')) if f.is_file()))
    d.rename(out)
z=T/(name+'.zip')
with zipfile.ZipFile(z,'x',zipfile.ZIP_DEFLATED) as zf:
    for f in sorted(out.rglob('*')):
        if f.is_file():zf.write(f,f.relative_to(T).as_posix())
print(out,sha(packet),z,z.stat().st_size)
