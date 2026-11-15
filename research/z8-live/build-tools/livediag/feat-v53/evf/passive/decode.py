#!/usr/bin/env python3
"""Decode bounded EVF events from unchanged LLG2 files. Absence is inconclusive."""
import argparse,hashlib,json,struct
from pathlib import Path
EVENT_BYTES=24
KEYS=['output','live_status','flags','view_lo','view_hi','view_id_state','surface_lo','surface_hi','root_id','root_group','root_output','root_target','backing_lo','backing_hi','dimensions','draw_entries','last_draw_result','core_submit_calls','callbacks','error','display_status','presents','present_fail','not_current_target_lost']
def decode(b):
    assert len(b)==32+32*24+128*24,('LLG2 size',len(b))
    h=struct.unpack_from('<8I',b);assert h[0]==0x32474c4c,'not LLG2'
    grouped={};source_counts={}
    for source,offset,capacity,head in [('crumbs',32,32,h[7]),('ring',32+32*24,128,h[1])]:
        source_counts[source]=0
        for i in range(max(0,head-capacity),head):
            t,k,a,c=struct.unpack_from('<QIIQ',b,offset+(i%capacity)*24)
            if not 61<=k<=68:continue
            source_counts[source]+=1
            slots=grouped.setdefault(t,{})
            vals=[a,c&0xffffffff,c>>32]
            assert k not in slots or slots[k]==vals,('inconsistent duplicate snapshot',t,k)
            slots[k]=vals
    samples=[]
    for t,slots in sorted(grouped.items()):
        if set(slots)!=set(range(61,69)):
            samples.append(dict(timestamp_us=t,complete=False,missing_events=sorted(set(range(61,69))-set(slots))));continue
        w=sum([slots[k] for k in range(61,69)],[]);r=dict(zip(KEYS,w));f=w[2]
        samples.append(dict(timestamp_us=t,complete=True,output=w[0],live_status=w[1],flags=dict(attached=bool(f&1),global_overlay=bool(f&2),shutdown=bool(f&4),loaded=bool(f&8),blocked=bool(f&16),gate=bool(f&32),descriptor_enabled=(f>>8)&255,descriptor_format=(f>>16)&255),view=hex(w[3]|w[4]<<32),view_id=w[5]&65535,view_state=w[5]>>16,surface=hex(w[6]|w[7]<<32),root=dict(id=w[8],group=w[9],output=w[10],target=w[11]),descriptor=dict(backing=hex(w[12]|w[13]<<32),width=w[14]&65535,height=w[14]>>16),draw_entries=w[15],last_draw_result='no_callback' if not w[15] else {0:'paint_failure',1:'paint_success',2:'blocked',0xffffffff:'skipped_gate_or_current'}.get(w[16],str(w[16])),core_submit_calls=w[17],callbacks=w[18],error=w[19],display_status=w[20],presents=w[21],present_fail=w[22],not_current=w[23]&65535,target_lost=w[23]>>16,raw_words=r))
    return dict(schema='v53.evf-passive-llg2/v1',sha256=hashlib.sha256(b).hexdigest(),file_time_us=h[2]|h[3]<<32,crumb_head=h[7],ring_head=h[1],diagnostic_events=source_counts,samples=samples,absence_is_inconclusive=True,physical_visibility_verified=False)
def main():
    p=argparse.ArgumentParser();p.add_argument('file',type=Path);p.add_argument('--output',type=Path);a=p.parse_args();v=decode(a.file.read_bytes());s=json.dumps(v,indent=2)+'\n'
    if a.output:a.output.write_text(s)
    else:print(s,end='')
if __name__=='__main__':main()
