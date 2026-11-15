"""Produce an evidence-labelled LCD layout review; does not patch firmware."""
import json
from pathlib import Path
from layout_data import *
b=SOURCE.read_bytes();t=rows(b,0);out=[]
# These names are proved by update routines/source. Unidentified groups retain IDs.
names={7:'Mode',30:'REC bitmap',32:'REC text',66:'Timer',42:'Histogram',35:'Audio meter',31:'Microphone',54:'Video format',56:'Video format',75:'Video FPS'}
for m in mapping(b):
 p=t.get(m['photo'][0]);v=t.get(m['video'][0]);
 if not any(r and 0<r['P'][1]<220 for r in (p,v)):continue
 out.append({'group':m['group'],'index':m['index'],'name':names.get(m['group'],'Unidentified status group '+str(m['group'])),'photo_id':hex(m['photo'][0]),'photo':p and p['P'],'video_id':hex(m['video'][0]),'video':v and v['P']})
Path(__file__).with_name('lcd-rows.json').write_text(json.dumps(out,indent=2)+'\n')
def candidate(i,r):
 if i in(0x1d9e,0x1d9f) or 0x1dad<=i<=0x1db1:return [r[0]+52,r[1]-132,r[2]+52,r[3]-132]
 if i==0x1f7f:return [r[0]+29,r[1]-132,r[2]+29,r[3]-132]
 return r
panels=[]
for field,title in [('photo','Photo anchors'),('video','Current video / elapsed'),('video','Candidate / elapsed'),('video','Candidate / timecode')]:
 entries=[];seen=set()
 for m in out:
  g=m['group'];r=m[field];i=int(m[field+'_id'],16)
  if not r or not 0<r[1]<220 or g==1:continue
  if (g,tuple(r)) in seen:continue
  seen.add((g,tuple(r)))
  if 'Candidate' in title:
   if g==66 and (('elapsed' in title and i!=0x1f7f) or ('timecode' in title and i!=0x1eda)):continue
   r=candidate(i,r)
  if 'Current' in title and g==66 and i!=0x1f7f:continue
  if g==30:continue # REC text is the wider conservative boundary
  if g==32 and m['index']!=0:continue
  if g==66 and i==0x1ed9:continue
  entries.append({'group':g,'id':hex(i),'rect':r,'label':('REC' if g==32 else 'TC 00:00:00:00' if i==0x1eda else '00:00:00' if i==0x1f7f else 'G'+str(g)),'new':r!=m[field]})
 panels.append({'title':title,'entries':entries})
Path(__file__).with_name('lcd-review-diagram.json').write_text(json.dumps(panels,indent=2)+'\n')
print(len(out),'mapped entries')
