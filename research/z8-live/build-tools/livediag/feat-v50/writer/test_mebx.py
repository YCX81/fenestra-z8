from pathlib import Path
import sys,hashlib,json,contextlib,io
HERE=Path(__file__).resolve().parent
OLD=HERE.parents[1]/'feat-mov'
sys.path[:0]=[str(OLD),str(OLD/'evidence')]
import check_ncdt,mov_fix,meta_handler_patch
fw=(OLD/'bg34.main').read_bytes()
assert hashlib.sha256(fw).hexdigest()==check_ncdt.FIRMWARE_HASH
(HERE/'parser/bg34.main').write_bytes(meta_handler_patch.apply(fw))
check_ncdt.ROOT=HERE/'parser'
d=(HERE.parents[1]/'feat-w5dec/hw-DSC_0028.MOV').read_bytes()
kept=mov_fix.rewrite(d,niko=True,audio_stco=True)
removed=mov_fix.rewrite(d,niko=True,audio_stco=True,remove_mebx=True)
moov=mov_fix.direct_child(mov_fix.parse(kept),b'moov')
t=next(t for t in moov.children if t.typ==b'trak' and mov_fix.handler(t)==b'meta')
h=t.one(b'hdlr');new=bytearray(h.data);new[8:12]=b'zzzz'
assert kept.count(h.encode())==1
unknown=kept.replace(h.encode(),mov_fix.Box(b'hdlr',bytes(new)).encode())
(HERE/'parser/mebx-kept.MOV').write_bytes(kept)
restored=(HERE/'parser/LIV_9013-META.MOV').read_bytes()
udta=mov_fix.direct_child(mov_fix.direct_child(mov_fix.parse(restored),b'moov').children,b'udta').encode()
cases=[('meta retained, narrow hook',kept,[0,3]),('unknown still rejected',unknown,[6]),('no metadata stock path',removed,[0,3]),('4K complete Nikon+Apple file',restored,[0,0,len(udta),0,1,0])]
capture=io.StringIO()
with contextlib.redirect_stdout(capture):check_ncdt.replay([],cases)
print(capture.getvalue(),end='')
(HERE/'parser/parser-test.json').write_text(json.dumps(dict(status='PASS',firmware_sha256=hashlib.sha256(fw).hexdigest(),hook_sha256=hashlib.sha256(meta_handler_patch.code()).hexdigest(),cases=[dict(name=n,sha256=hashlib.sha256(d).hexdigest(),expected=v) for n,d,v in cases],output=capture.getvalue(),scope='Actual firmware instructions in QEMU virt; not full native playback or camera'),indent=2)+'\n')
