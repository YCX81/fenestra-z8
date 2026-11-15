from PIL import Image,ImageDraw,ImageFont
import json,sys
D=json.load(open(sys.argv[1]));S=.6;W=1240;H=900
im=Image.new('RGB',(W,H),'#101827');d=ImageDraw.Draw(im)
font='/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf';f=ImageFont.truetype(font,13);title=ImageFont.truetype(font,20);small=ImageFont.truetype(font,10)
d.text((24,15),'EVF portrait layout / native photo anchors + video status rows',font=title,fill='white')
d.text((24,44),'Logical geometry diagram. Not a camera screenshot; labels are placeholders, native fonts are unchanged.',font=f,fill='#b4c1d6')
for panel,entries in enumerate(D):
 ox,oy=24+panel*612,100
 d.text((ox,72),'Normal display' if panel==0 else 'Small display (native EVF inset)',font=title,fill='white')
 d.rectangle((ox,oy,ox+960*S,oy+1280*S),fill='#182235',outline='#718096',width=2)
 for e in entries:
  r=e['rect'];xy=(ox+r[0]*S,oy+r[1]*S,ox+r[2]*S,oy+r[3]*S);color='#efb35b' if e['new'] else '#72849e'
  if e['label']=='REC':color='#f57180'
  d.rectangle(xy,outline=color,width=2 if e['new'] else 1)
  if xy[2]-xy[0]>20:d.text((xy[0]+2,xy[1]+2),e['label'],font=small,fill=color)
 d.text((ox+100,oy+460),'Live-view image area',font=title,fill='#40546b')
 d.text((ox+100,oy+490),'960 x 1280 logical canvas',font=f,fill='#40546b')
im.save(sys.argv[2])
