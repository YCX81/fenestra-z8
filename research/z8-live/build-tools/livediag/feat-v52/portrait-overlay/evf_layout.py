"""v52 EVF portrait authoring. Native photo twins retain their own EVF geometry.
Video-only groups use the free center of native EVF status rows; source fonts and
compound control geometry are retained. This is authored layout, not recovered Nikon art.
"""
from layout_data import *
def move(rect,source,dest):
 dx,dy=dest[0]-source[0],dest[1]-source[1]
 return rect[0]+dx,rect[1]+dy,rect[2]+dx,rect[3]+dy
def plan(b):
 tables=[rows(b,i) for i in range(8)];maps=mapping(b);by={(m['group'],m['index']):m for m in maps};out=[]
 for i in (2,3):
  small=i==3;sx,sy=(66,74) if small else(0,0)
  def twin(g,j):return tables[i][by[g,j]['photo'][i]]['P']
  def via(g,j,rect):
   m=by[g,j];return move(rect,tables[i][m['video'][i]]['L'][:2],twin(g,j)[:2])
  for m in maps:
   g,j=m['group'],m['index'];v=m['video'][i];p=m['photo'][i];r=tables[i].get(v)
   if not r or not any(r['L']) or any(r['P']):continue
   pr=tables[i].get(p);L=r['L'];kind='native-photo-twin'
   if pr and any(pr['P']):P=pr['P']
   else:
    kind='authored-video-only'
    if g==6:P=move(L,tables[i][by[4,0]['video'][i]]['L'][:2],twin(4,0)[:2])
    elif g==14:P=via(14,1,L)
    elif g==59:P=move(L,(294,784 if small else 856),(53+sx,1032-(66 if small else 0)))
    elif g==65:P=via(65,0,L)
    elif g in (30,32):P=move(L,(294,136 if small else 64),(100+sx,60+sy))
    elif g==66:P=move(L,(560,142 if small else 70),(184+sx,66+sy))
    elif g==33:P=move(L,(865,137 if small else 65),(774-sx,61+sy))
    elif g==38:P=move(L,(829,138 if small else 66),(742-sx,62+sy))
    elif g==29:P=move(L,(753,136 if small else 64),(592+sx,112+sy))
    elif g==52:P=move(L,(158,136 if small else 64),(100+sx,112+sy))
    elif g in (54,56):P=move(L,(386,136 if small else 64),(228+sx,112+sy))
    elif g==75:P=move(L,(466,136 if small else 64),(308+sx,112+sy))
    elif g==34:P=move(L,(986 if small else 970,80 if small else 8),(402+sx,112+sy))
    elif g==77:P=move(L,(1003,136 if small else 64),(478+sx,112+sy))
    elif g==68:P=move(L,(306 if small else 285,80 if small else 8),(100+sx,164+sy))
    elif g==73:P=move(L,(1138 if small else 1140,80 if small else 8),(802-sx,164+sy))
    elif g==47:P=move(L,(396 if small else 150,80 if small else 8),(100+sx,216+sy))
    elif g==49:P=move(L,(190 if small else 106,784 if small else 856),(9+sx,968-(66 if small else 0)))
    elif g==39:P=move(L,(54,82 if small else 10),(162+sx,396+sy))
    elif g in (31,35) and (g==31 or j<34):P=move(L,(518,788) if small else(434,860),(376,1102) if small else(320,1168))
    elif g==35:P=move(L,(282,694),(108+sx,438+sy))
    elif g==71 and j<2:P=move(L,(232,691),(330,1164))
    elif g==71:P=move(L,(296,650),(122+sx,394+sy))
    elif g==42:
     # Use the original small-EVF waveform's supported 744x376 geometry.
     P=(108,400,852,776)
    else:raise ValueError(('unhandled',i,g,j,hex(v)))
   assert 0<=P[0]<P[2]<=960 and 0<=P[1]<P[3]<=1280,(i,g,j,P)
   out.append(dict(table=i,group=g,index=j,id=v,address=r['address'],old=r['P'],new=P,kind=kind,photo_id=p))
 return out
if __name__=='__main__':
 import json,collections
 o=plan(SOURCE.read_bytes());Path(__file__).with_name('evf-layout-plan.json').write_text(json.dumps(o,indent=2)+'\n');print(len(o),collections.Counter((x['table'],x['kind']) for x in o))
