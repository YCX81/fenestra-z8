# Live badge 112x44: concentric rings + LIVE lettering, white on black; busy = left-to-right dark-to-bright sweep.
import math
from PIL import Image, ImageDraw, ImageFont
W,H,S=112,44,8
def base():
    im=Image.new('L',(W*S,H*S),0); d=ImageDraw.Draw(im); cx,cy=22*S,22*S
    def ring(r,w): d.ellipse([cx-r,cy-r,cx+r,cy+r],outline=255,width=w)
    d.ellipse([cx-4.2*S,cy-4.2*S,cx+4.2*S,cy+4.2*S],fill=255)
    ring(10.5*S,int(2.2*S))
    r=18*S
    for i in range(36):
        a=2*math.pi*i/36; x,y=cx+r*math.cos(a),cy+r*math.sin(a); q=1.15*S
        d.ellipse([x-q,y-q,x+q,y+q],fill=255)
    # LIVE from strokes, same weight as the ring
    w=int(2.4*S); top,bot=13*S,31*S; x=47*S
    d.line([(x,top),(x,bot-w/2),(x+9*S,bot-w/2)],fill=255,width=w)
    x=61*S; d.line([(x,top),(x,bot)],fill=255,width=w)
    x=67*S; d.line([(x,top),(x+6*S,bot)],fill=255,width=w); d.line([(x+6*S,bot),(x+12*S,top)],fill=255,width=w)
    x=84*S; d.line([(x,top),(x,bot)],fill=255,width=w)
    for y in (top+w/2,(top+bot)/2,bot-w/2): d.line([(x,y),(x+10*S,y)],fill=255,width=w)
    return im.resize((W,H),Image.LANCZOS)
b=base(); b.save('live-112x44.png'); b.resize((W*6,H*6),Image.NEAREST).save('live-x6.png')
N=24; frames=[]
for f in range(N):
    head=(f/N)*(W+60)-30   # bright band centre moves left to right, then wraps
    px=b.load(); o=Image.new('L',(W,H),0); q=o.load()
    for y in range(H):
        for x in range(W):
            d=abs(x-head); g=0.25+0.75*math.exp(-(d/22)**2)
            q[x,y]=int(px[x,y]*g)
    frames.append(o)
frames[0].save('live-busy-x1.png')
big=[f.resize((W*6,H*6),Image.NEAREST).convert('P') for f in frames]
big[0].save('live-busy-x6.gif',save_all=True,append_images=big[1:],duration=60,loop=0)
sheet=Image.new('L',(W*6,H*6*2+12),0); sheet.paste(b.resize((W*6,H*6),Image.NEAREST),(0,0)); sheet.paste(frames[10].resize((W*6,H*6),Image.NEAREST),(0,H*6+12)); sheet.save('preview.png')
