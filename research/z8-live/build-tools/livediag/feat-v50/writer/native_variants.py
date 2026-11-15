"""Single-variable native-player fixtures. Never production donor metadata."""
from pathlib import Path
import sys,copy,json,hashlib
H=Path(__file__).resolve().parent;sys.path.insert(0,str(H.parents[1]/'feat-mov'))
import mov_fix as m
base=(H/'parser/HOST_4K.MOV').read_bytes();donor=(H/'parser/LIV_9013-META.MOV').read_bytes()
reports=[]
for name in ['donor-ncdt','metadata-last','no-metadata']:
 roots=m.parse(base);moov=m.direct_child(roots,b'moov')
 if name=='donor-ncdt':
  udta=m.direct_child(m.direct_child(m.parse(donor),b'moov').children,b'udta')
  moov.children=[copy.deepcopy(udta) if x.typ==b'udta' else x for x in moov.children]
 elif name=='metadata-last':
  others=[x for x in moov.children if not(x.typ==b'meta' or x.typ==b'trak' and m.handler(x)==b'meta')]
  metadata=[x for x in moov.children if x not in others];moov.children=others+metadata
 else:moov.children=[x for x in moov.children if not(x.typ==b'meta' or x.typ==b'trak' and m.handler(x)==b'meta')]
 out=b''.join(x.encode() for x in roots)
 before=m.inspect(base);after=m.inspect(out)
 for kind in ['vide','soun']:
  assert next(x['sample_sha256'] for x in before['tracks'] if x['handler']==kind)==next(x['sample_sha256'] for x in after['tracks'] if x['handler']==kind)
 p=H/'parser'/('native-'+name+'.MOV');p.write_bytes(out)
 reports.append(dict(name=name,bytes=len(out),sha256=hashlib.sha256(out).hexdigest(),base_sha256=hashlib.sha256(base).hexdigest(),scope='single-variable native UI experiment, not production'))
(H/'parser/native-variants.json').write_text(json.dumps(reports,indent=2)+'\n');print(json.dumps(reports,indent=2))
