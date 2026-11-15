"""Decode v52 passive shutter events from a user-supplied LIVELOG.BIN copy."""
import argparse,json,struct
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('log',type=Path);a=p.parse_args();b=a.log.read_bytes()
assert len(b)>=32+32*24 and b[:4]==b'LLG2','not LLG2'
names=['driver','service','sound_running','stock_mute','inhibit','prepared','capture_phase','audio_active']
points={0:'full_press',1:'prepare_message',2:'fire_message',3:'mute_gate'}
rows=[]
for off in range(32,len(b)-23,24):
 t,event,arg,data=struct.unpack_from('<QIIQ',b,off)
 if event!=60 or not t:continue
 row={'time_us':t,'point':points.get(arg&255,str(arg&255)),'incoming':arg>>8&255,'prepare_state':arg>>16&255,'fire_state':arg>>24&255}
 row.update({name:data>>(8*i)&255 for i,name in enumerate(names)})
 if row not in rows:rows.append(row)
print(json.dumps(sorted(rows,key=lambda x:x['time_us']),ensure_ascii=False,indent=2))
