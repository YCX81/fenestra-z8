from PIL import Image,ImageDraw,ImageFont
import json,sys
D=json.load(open(sys.argv[1]));S=1.2;W=1240;H=750
im=Image.new('RGB',(W,H),'#101827');d=ImageDraw.Draw(im)
fpath='/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf';f=ImageFont.truetype(fpath,12);t=ImageFont.truetype(fpath,20)
d.text((24,12),'LCD portrait: native photo rows / current video / proposed minimal change',font=t,fill='white')
d.text((24,42),'480 x 720 logical canvas: upper five 44-pixel rows shown. Geometry diagram, not a camera screenshot.',font=f,fill='#b4c1d6')
for p,x in enumerate(D):
 ox=24+(p%2)*610;oy=110+(p//2)*324
 d.text((ox,oy-30),x['title'],font=t,fill='white')
 d.rectangle((ox,oy,ox+480*S,oy+224*S),fill='#182235',outline='#718096',width=2)
 for y in (6,50,94,138,182):d.line((ox,oy+y*S,ox+480*S,oy+y*S),fill='#29364c')
 for e in x['entries']:
  r=e['rect'];xy=(ox+r[0]*S,oy+r[1]*S,ox+r[2]*S,oy+r[3]*S);color='#efb35b' if e['new'] else '#72849e'
  if e['group']==32:color='#f57180'
  d.rectangle(xy,outline=color,width=2 if e['new'] else 1)
  if xy[2]-xy[0]>30:d.text((xy[0]+2,xy[1]+5),e['label'],font=f,fill=color)
d.text((24,730),'G numbers preserve unknown widget identity; rectangles include alternative native states, not a claim that all are simultaneously visible.',font=f,fill='#b4c1d6')
im.save(sys.argv[2])
