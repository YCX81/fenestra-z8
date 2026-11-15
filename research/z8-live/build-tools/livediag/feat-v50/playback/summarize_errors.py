from pathlib import Path
import json,struct,hashlib,sys
B=Path.home()/'.local/share/nikon-z8-qemu/live-audio/regress/runs'
records=[]
for n in sys.argv[1:]:
 d=B/n;x=json.loads((d/'result.json').read_text());cs=x['live_checkpoints'];p=d/'lv-observer.dat';b=p.read_bytes();h=struct.unpack_from('<12Q',b)
 ev=[]
 for i in range(max(0,h[4]-65536),h[4]):
  r=struct.unpack_from('<8Q2I',b,96+(i%65536)*72)
  if r[-1]>=200:
   before=next((c for c in reversed(cs) if c.get('lv_diagnostics',{}).get('host_ns',0)<r[1]),None)
   after=next((c for c in cs if c.get('lv_diagnostics',{}).get('host_ns',0)>r[1]),None)
   ev.append(dict(seq=r[0],host_ns=r[1],after_checkpoint=before['label'] if before else None,before_checkpoint=after['label'] if after else None,pc=hex(r[2]),args=list(map(hex,r[3:7])),lr=hex(r[7]),cpu=r[-2],kind=r[-1]))
 records.append(dict(name=n,error=x.get('error'),trace_complete=x.get('capture_trace',{}).get('complete'),firmware_sha256=x['live_a']['executed_main_sha256'],qemu_sha256=x['qemu_sha256'],driver_sha256=hashlib.sha256((d/'run_live.py').read_bytes()).hexdigest(),observer=dict(sha256=hashlib.sha256(b).hexdigest(),head=h[4],overwritten=h[5],reg_errors=h[8]),events=ev,history=cs[-1].get('stock_error_history'),before=x.get('pb_recovery_before'),after=x.get('pb_recovery_after'),runtime={k:cs[-1].get('runtime',{}).get(k) for k in ['capture.irq_seq','stream.encoded','transaction_count','stream.error']},inputs_unchanged=x.get('input_unchanged'),owned_qemu_stopped=x.get('owned_qemu_stopped')))
print(json.dumps(records,indent=2))
