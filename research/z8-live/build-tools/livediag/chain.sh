#!/bin/bash
# Diagnostic chain on win-lab: Live layer (build-diag) -> playback layer -> USB default on -> GUI pool fix.
set -euo pipefail
L=$HOME/.local/share/nikon-z8-qemu/live-audio; cd $L
PY=$HOME/.local/share/nikon-z8-qemu/venv/bin/python
IN=${1:-build-diag}; OUT=${2:-chain-diag}
rm -rf $OUT; mkdir -p $OUT/pb $OUT/usb $OUT/pool
python3 pbin/relive_pb.py pbflat/src/live-playback-ui $IN/live-fixed.emulation-only.main $IN/changed-ranges.json $OUT/pb --defs='{"fx_glyph":"0x401f0000","fx_memmove":"0x401f0100"}' | tail -1
python3 w5in/usb_default_on.py $OUT/pb/live-fixed.emulation-only.main $OUT/usb/usb.main | tail -1
$PY - $OUT/pb $OUT/usb <<'P'
import json,hashlib,sys
from pathlib import Path
pb,usb=map(Path,sys.argv[1:]);a=(pb/'live-fixed.emulation-only.main').read_bytes();b=(usb/'usb.main').read_bytes()
m=json.loads((pb/'changed-ranges.json').read_text());r=[];i=0;n=len(a)
while i<n:
    if a[i]!=b[i]:
        j=i
        while j<n and a[j]!=b[j]:j+=1
        r.append(dict(address=hex(0x40160000+i),size=j-i,purpose='usb debug default on',expected=a[i:j].hex(),replacement=b[i:j].hex()));i=j
    else:i+=1
m['ranges']=m['ranges']+r;m['output_sha256']=hashlib.sha256(b).hexdigest();(usb/'changed-ranges.json').write_text(json.dumps(m,indent=1));print('usb ranges',len(r))
P
$PY w5in/gui_pool_fix.py $OUT/usb/usb.main $OUT/pool/pool.main | tail -1
sha256sum $OUT/pb/*.main $OUT/usb/usb.main $OUT/pool/pool.main | cut -c1-16,65-
