"""Code-drawn geometry review with original PNG assets, not a camera capture."""
from pathlib import Path
import io,json,struct
from PIL import Image,ImageDraw,ImageFont
from layout_data import BASE,rows,mapping
P=Path(__file__).parent
old=(P/'v52g.main').read_bytes();new=(P/'candidate.main').read_bytes()
font='/System/Library/Fonts/Supplemental/Arial.ttf'
f=ImageFont.truetype(font,15);title=ImageFont.truetype(font,22);small=ImageFont.truetype(font,11)
out=Image.new('RGB',(1440,1430),'#101827');d=ImageDraw.Draw(out)
d.text((24,16),'v53 portrait status layout — actual resource IDs and native geometry',font=title,fill='white')
d.text((24,47),'CODE-DRAWN REVIEW, NOT A CAMERA SCREENSHOT. Only affected status groups shown; fonts for timer annotations are illustrative.',font=f,fill='#b9c7dc')
maps=mapping(old)
def rr(b,t,g,j):
    m=next(m for m in maps if(m['group'],m['index'])==(g,j))
    return rows(b,t).get(m['video'][t])
def asset(i):
    _,n,_,ptr=struct.unpack_from('<IIQQ',old,0x431d96d0-BASE+24*i)
    return Image.open(io.BytesIO(old[ptr-BASE:ptr-BASE+n])).convert('RGBA')
def bbox(b,t,g,cond=lambda j:True):
    rs=[rows(b,t)[m['video'][t]]['P'] for m in maps if m['group']==g and cond(m['index']) and m['video'][t] in rows(b,t)]
    return (min(r[0]for r in rs),min(r[1]for r in rs),max(r[2]for r in rs),max(r[3]for r in rs))
panels=[]
for row,b in enumerate((old,new)):
    for col,table in enumerate((0,2,3)):
        w,h=(480,720) if table==0 else(960,1280)
        scale=min(460/w,560/h)
        # LCD is proportionally displayed shorter in height; logical units are
        # not claimed to be physical pixels or equal visual angles.
        ox=20+col*476;oy=112+row*650
        d.text((ox,oy-32),('v52 / 'if row==0 else'v53 / ')+('LCD'if table==0 else'EVF normal'if table==2 else'EVF small'),font=title,fill='white')
        canvas=Image.new('RGBA',(w,h),'#223047');cd=ImageDraw.Draw(canvas)
        entries=[]
        for g,fg,bg in ((34,0x768,0x769),(37,0x84d,0x9a7),(73,0xa04,0xa62)):
            for j,i in ((0,bg),(1,fg)):
                r=rr(b,table,g,j)
                if not r:continue
                box=r['P'];im=asset(i);x=box[0]+(box[2]-box[0]-im.width)//2;y=box[1]+(box[3]-box[1]-im.height)//2
                canvas.alpha_composite(im,(x,y));entries.append(dict(group=g,row=j,resource=hex(i),rect=box,paint=[x,y,x+im.width,y+im.height]))
        def rect(box,label,color):
            cd.rectangle(box,outline=color,width=2);cd.text((box[0]+2,box[1]+2),label,font=f,fill=color)
        rect(bbox(b,table,32),'REC','#ff7888')
        rect(rr(b,table,66,2)['P'],'00:00:00','#ffbd66')
        rect(bbox(b,table,33),'remaining','#75d7d4')
        rect(bbox(b,table,38),'card','#75d7d4')
        rect(bbox(b,table,31),'mic','#8095ac')
        rect(bbox(b,table,35,lambda j:j<34),'audio','#8095ac')
        rect(bbox(b,table,19),'stock count','#8095ac')
        # A separate dotted sample at row5 labels the alternate timecode;
        # do not imply TC and elapsed are displayed simultaneously.
        tc=rr(b,table,66,1)['P']
        if row==1:
            cd.text((tc[0],tc[1]+35),'TC alternative at row 5',font=f,fill='#91a3bc')
            right=max(e['paint'][2] for e in entries if e['group']==37)
            cd.line((right+1,40,right+1,330 if table else 180),fill='#5ce0a0',width=1)
        canvas=canvas.convert('RGB').resize((int(w*scale),int(h*scale)),Image.Resampling.NEAREST)
        # Reserve a 470px panel; LCD's 480px source gets a modest 0.95 fit.
        if canvas.width>460:canvas=canvas.resize((460,round(canvas.height*460/canvas.width)),Image.Resampling.NEAREST)
        out.paste(canvas,(ox,oy))
        d.rectangle((ox,oy,ox+canvas.width,oy+canvas.height),outline='#617086')
        panels.append(dict(table=table,version=52+row,entries=entries))
d.text((24,1401),'Green line: common label-plate edge. Elapsed and long TC are alternatives. All native PNGs and rectangle sizes are unchanged.',font=f,fill='#b9c7dc')
out.save(P/'layout-review.png');(P/'layout-review.json').write_text(json.dumps(panels,indent=2)+'\n')
