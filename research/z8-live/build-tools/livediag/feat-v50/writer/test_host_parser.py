"""Execute stock C3.11 parsing and all four NCDT image queries on C writer output."""
from pathlib import Path
import sys,json,hashlib,contextlib,io
H=Path(__file__).resolve().parent;OLD=H.parents[1]/'feat-mov'
sys.path[:0]=[str(OLD),str(OLD/'evidence')]
import check_ncdt as c,mov_fix as m,meta_handler_patch
fw=(OLD/'bg34.main').read_bytes();assert hashlib.sha256(fw).hexdigest()==c.FIRMWARE_HASH
(H/'parser/bg34.main').write_bytes(meta_handler_patch.apply(fw));c.ROOT=H/'parser'
d=(H/'host-sample.MOV').read_bytes();udta=m.direct_child(m.direct_child(m.parse(d),b'moov').children,b'udta').encode()
images=[(160,120,0),(1280,720,1),(640,360,2),(1920,1080,3)]
cap=io.StringIO()
try:
 with contextlib.redirect_stdout(cap):c.replay([('C writer dynamic NCDT',udta,1,0,images)],[('C writer full MOV',d,[0,0,len(udta),0,1,0])])
except Exception:
 print(cap.getvalue(),end='');raise
print(cap.getvalue(),end='')
(H/'parser/host-parser-test.json').write_text(json.dumps(dict(status='PASS',sample_sha256=hashlib.sha256(d).hexdigest(),output=cap.getvalue(),scope='Actual C3.11 parser/NCDT queries under QEMU virt; not native playback'),indent=2)+'\n')
