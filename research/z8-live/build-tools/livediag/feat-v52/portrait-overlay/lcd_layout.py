"""Minimal approved LCD video portrait reordering. Long timecode keeps row 5."""
from layout_data import rows,mapping
REC={0x1d9e,0x1d9f,0x1dad,0x1dae,0x1daf,0x1db0,0x1db1}
def plan(b):
 out=[];maps=mapping(b)
 for table in (0,1):
  t=rows(b,table)
  for m in maps:
   i=m['video'][table]
   if i not in REC and i!=0x1f7f:continue
   r=t[i];L=r['P'];dx=52 if i in REC else 29;dy=-132
   P=(L[0]+dx,L[1]+dy,L[2]+dx,L[3]+dy)
   assert 146<=P[0]<P[2]<=360 and 50<=P[1]<P[3]<=86
   out.append(dict(table=table,group=m['group'],index=m['index'],id=i,address=r['address'],old=L,new=P,kind='lcd-rec-elapsed-row2',photo_id=m['photo'][table]))
 return out
