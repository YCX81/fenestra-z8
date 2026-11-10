#!/usr/bin/env python3
"""Image-only checks and reproducible stock/old/new pixel measurements.
No firmware sources, headers, compilers, USB or device access.
"""
import hashlib
import json
import shutil
import struct
import zlib
import xml.etree.ElementTree as ET
from pathlib import Path
import icon32_generate as art

ROOT = Path(__file__).resolve().parent

def read_png(path):
    data = path.read_bytes()
    w,h = struct.unpack_from('>II',data,16)
    assert data[24:26] == bytes((8,6)), 'expected RGBA8'
    stream = bytearray(); pos = 8
    while pos < len(data):
        n = struct.unpack_from('>I',data,pos)[0]
        if data[pos+4:pos+8] == b'IDAT':
            stream.extend(data[pos+8:pos+8+n])
        pos += 12+n
    raw=zlib.decompress(stream); stride=w*4+1
    assert len(raw)==stride*h and all(raw[y*stride]==0 for y in range(h))
    return w,h,b''.join(raw[y*stride+1:(y+1)*stride] for y in range(h))

def check_assets():
    assets=ROOT/'icon32-assets'
    cfg=json.loads((assets/'geometry.json').read_text())
    vector=ET.parse(assets/'shoot-busy.svg').getroot()
    motion=vector.find('.//{*}animateTransform')
    centers=[float(pair.split()[0]) for pair in motion.get('values').split(';')]
    assert len(centers)==24 and all(a<b for a,b in zip(centers,centers[1:]))
    assert motion.get('dur')=='1.44s'
    report={'scope':'assets only; firmware source/build checks NOT run','owners':{}}
    for prefix in ('shoot','pb'):
        w,h,static=read_png(assets/(prefix+'.png'))
        assert (w,h)==(cfg[prefix]['width'],cfg[prefix]['height'])
        coverage=art.artwork(w,h,cfg['raster']=='binary')
        assert static==art.rgba_frame(coverage,w)
        alpha=static[3::4]
        assert not any(alpha[:w]+alpha[-w:]+alpha[::w]+alpha[w-1::w])
        fixed=[i for i in range(w*h) if static[4*i]==0]
        frames=[]
        for phase in range(art.FRAMES):
            fw,fh,rgba=read_png(assets/f'{prefix}-busy-{phase:02d}.png')
            assert (fw,fh)==(w,h)
            assert rgba==art.rgba_frame(coverage,w,phase,cfg['raster']=='binary')
            assert rgba[3::4]==alpha
            assert all(rgba[4*i:4*i+4]==static[4*i:4*i+4] for i in fixed)
            frames.append(rgba)
        # Independently observe brightness over time at real white cores.
        # The rise, maximum and fall must traverse dot,L,I,V,E in that order.
        probes=[('dot',20,22),('L',43,22),('I',63,22),('V',77,35),('E',91,22)]
        peaks=[]; traces={}
        for label,x,y in probes:
            i=4*(y*w+x)
            assert static[i:i+4]==b'\xff'*4
            trace=[frame[i] for frame in frames]
            peak=trace.index(max(trace)); peaks.append(peak)
            assert min(trace)<=65 and max(trace)>=245
            assert all(a<=b for a,b in zip(trace[:peak],trace[1:peak+1]))
            assert all(a>=b for a,b in zip(trace[peak:],trace[peak+1:]))
            traces[label]={'xy':[x,y],'peak_phase':peak,'min':min(trace),'max':max(trace),'trace':trace}
        assert all(a<b for a,b in zip(peaks,peaks[1:]))
        report['owners'][prefix]={'size':[w,h],'busy_frames':len(frames),'alpha_and_black_fixed':True,
            'sweep_probes':traces,'static_sha256':hashlib.sha256(static).hexdigest()}
    (assets/'asset-check-results.json').write_text(json.dumps(report,indent=2)+'\n')
    print('PASS icon32_check.py --assets-only: both static images + 48 busy PNGs; fixed alpha/black; left-to-right rise/peak/fall')
    print('Peak phases (dot,L,I,V,E):',peaks)

def runs(values):
    out=[]
    for x in values:
        if out and out[-1][1]==x:out[-1][1]=x+1
        else:out.append([x,x+1])
    return out

def measure_and_compare(ffmpeg, assets, comparison=None):
    import icon32_visual_check as v
    stock=v.decode(ffmpeg,v.pinned(v.ARCHIVE/'before-playback-shooting.png'),'rgb24')
    def stock_px(x,y):return stock[(y*1024+x)*3:(y*1024+x+1)*3]
    result={'thresholds':{'white':'min RGB >= 224; RGBA uses white coverage RGB*alpha/255',
                          'black':'max RGB < 24 on stock; opaque black on assets'},
            'stock_sha256':v.PINNED['before-playback-shooting.png'],'stock':{},'assets':{}}
    for name,box,y in [('A',(20,10,68,59),36),('MF',(278,5,346,62),42),('FINE',(796,72,893,120),88),('VIEW+camera',(4,72,103,121),96)]:
        bounds=v.bright_bounds(stock,1024,box,3)
        columns=runs(x for x in range(box[0],box[2]) if any(min(stock_px(x,j))>=224 for j in range(box[1],box[3])))
        white=runs(x for x in range(box[0],box[2]) if min(stock_px(x,y))>=224)
        black=runs(x for x in range(box[0],box[2]) if max(stock_px(x,y))<24)
        result['stock'][name]={'bright_bounds':bounds,'height':bounds[3]-bounds[1],
            'sample_y':y,'white_runs':white,'black_runs':black,'column_runs':columns,
            'ink_gaps':[b[0]-a[1] for a,b in zip(columns,columns[1:])]}
    for name,labels in [('FINE','FINE'),('MF','MF')]:
        entry=result['stock'][name]; letters={}
        for label,(left,right) in zip(labels,entry['column_runs']):
            bounds=v.bright_bounds(stock,1024,(left,entry['bright_bounds'][1],right,entry['bright_bounds'][3]),3)
            width,height=bounds[2]-bounds[0],bounds[3]-bounds[1]
            letters[label]={'bounds':bounds,'width':width,'height':height,'width_height_ratio':width/height}
        entry['letters']=letters
    for name,path in [('old',assets/'previous/shoot.png'),('second',assets/'second/shoot.png'),('third',assets/'shoot.png')]:
        w,h,rgba=read_png(path)
        coverage=bytes((rgba[i]*rgba[i+3]+127)//255 for i in range(0,len(rgba),4))
        box=v.bright_bounds(coverage,w,(0,0,w,h),1)
        y=22
        white=runs(x for x in range(w) if coverage[y*w+x]>=224)
        black=runs(x for x in range(w) if rgba[4*(y*w+x)]==0 and rgba[4*(y*w+x)+3]==255)
        columns=runs(x for x in range(w) if any(coverage[j*w+x]>=224 for j in range(h)))
        letter_boxes = ([(37,0,53,44),(53,0,59,44),(59,0,79,44),(79,0,100,44)] if name=='old' else
                        [(44,0,67,44),(68,0,79,44),(80,0,112,44),(113,0,138,44)] if name=='second' else
                        [(40,0,60,44),(60,0,68,44),(68,0,88,44),(88,0,110,44)])
        letters={}
        for label,roi in zip('LIVE',letter_boxes):
            bounds=v.bright_bounds(coverage,w,roi,1)
            sample_y=22 if label=='V' else 16
            strokes=runs(x for x in range(roi[0],roi[2]) if coverage[sample_y*w+x]>=224)
            widths=[r-l for l,r in strokes]
            letters[label]={'bounds':bounds,'width':bounds[2]-bounds[0],'height':bounds[3]-bounds[1],
                            'width_height_ratio':(bounds[2]-bounds[0])/(bounds[3]-bounds[1]), 'sample_y':sample_y,
                            'white_runs':strokes,'stroke_widths':widths,
                            'stroke_height_ratios':[round(n/(bounds[3]-bounds[1]),4) for n in widths]}
        contour=v.bright_bounds(bytes(255 if a else 0 for a in rgba[3::4]),w,(0,0,w,h),1)
        result['assets'][name]={'alpha_bounds':contour,'visible_width':contour[2]-contour[0],'letters':letters,'size':[w,h],'bright_bounds':box,'sample_y':y,
            'white_runs':white,'black_runs':black,'column_runs':columns,
            'ink_gaps':[b[0]-a[1] for a,b in zip(columns,columns[1:])]}
    third=result['assets']['third']; fine=result['stock']['FINE']['letters']
    for label in 'LVE':
        assert abs(third['letters'][label]['width_height_ratio']/fine['E']['width_height_ratio']-1)<.03
    assert third['letters']['I']['width']==5  # 5px stroke constraint; stock I is only 4px.
    for label in 'LIVE':
        assert third['letters'][label]['stroke_widths']==([5,5] if label=='V' else [5])
    view=result['stock']['VIEW+camera']['bright_bounds']
    cfg=json.loads((assets/'geometry.json').read_text()); box=third['bright_bounds']
    x=max(0,cfg['shoot']['center_x']-(box[0]+box[2]+1)//2)
    contour=third['alpha_bounds']; placed=[x+contour[0],x+contour[2]]
    assert placed[0]>=0 and placed[1]<=128, 'third badge crosses first-column boundary'
    assert max(abs(placed[0]-view[0]),abs(placed[1]-view[2]))<=10, 'badge no longer approximately aligns with VIEW row'
    result['layout']={'preview_x':x,'placed_alpha_x':placed,'view_bright_x':[view[0],view[2]],
        'view_bright_width':view[2]-view[0],'visible_vs_view_ratio':third['visible_width']/(view[2]-view[0]),
        'column_right':128,'right_clearance':128-placed[1]}
    (assets/'stroke-measurements.json').write_text(json.dumps(result,indent=2)+'\n')
    for group in ('stock','assets'):
        for name,entry in result[group].items():
            print(f'MEASURE {name}: {json.dumps(entry)}')
    if not comparison:return
    # Same native stock pixels above three columns: OLD / SECOND / THIRD.
    # Rows below are medium grey and dark; a separate sheet shows light/dark sweeps.
    width,height=570,310
    sheet=bytearray([30])*(width*height*3)
    def crop(box):
        l,t,r,b=box
        return b''.join(stock_px(x,y)+b'\xff' for y in range(t,b) for x in range(l,r)),r-l,b-t
    for col in (0,190,380):
        for box,x,y in [((20,10,68,59),8,8),((280,10,342,59),70,8),((170,10,224,59),134,8),((795,72,893,121),8,65),((4,72,103,121),8,120)]:
            rgba,w,h=crop(box);v.stamp(sheet,width,col+x,y,rgba,w,h)
    for col,label in [(0,'old'),(190,'second'),(380,'third')]:
        path=assets/'previous/shoot.png' if label=='old' else assets/'second/shoot.png' if label=='second' else assets/'shoot.png'
        w,h,rgba=read_png(path)
        for y,bg in [(178,100),(234,12)]:
            for j in range(y-2,y+46):
                start=(j*width+col)*3;sheet[start:start+186*3]=bytes([bg])*(186*3)
            v.stamp(sheet,width,col+8,y,rgba,w,h)
    glyphs={'O':['111','101','101','101','111'],'L':['100','100','100','100','111'],
        'D':['110','101','101','101','110'],'V':['101','101','101','101','010'],
        '2':['111','001','111','100','111'],'3':['111','001','111','001','111']}
    for col,label in [(0,'OLD'),(190,'V2'),(380,'V3')]:
        for k,char in enumerate(label):
            for j,row in enumerate(glyphs[char]):
                for i,bit in enumerate(row):
                    if bit=='1':
                        for dy in range(2):
                            for dx in range(2):
                                at=((294+j*2+dy)*width+col+8+k*8+i*2+dx)*3
                                sheet[at:at+3]=bytes([240])*3
    def save_rgb(path,rgb,w,h,scale=1):
        rgba=b''.join(rgb[3*((y//scale)*w+x//scale):3*((y//scale)*w+x//scale)+3]+b'\xff'
                      for y in range(h*scale) for x in range(w*scale))
        art.png(path,w*scale,h*scale,rgba)
    save_rgb(comparison,sheet,width,height)
    save_rgb(comparison.with_name(comparison.stem+'-2x.png'),sheet,width,height,2)
    # Light/dark/backdrop and actual busy phases, entirely at native scale.
    w,h,normal=read_png(assets/'shoot.png'); pw=6*(w+8);ph=3*(h+8)
    contact=bytearray([96])*(pw*ph*3)
    for row,bg in enumerate((12,96,245)):
        contact[row*(h+8)*pw*3:(row+1)*(h+8)*pw*3]=bytes([bg])*((h+8)*pw*3)
        for col,phase in enumerate((None,7,10,13,16,19)):
            rgba=normal if phase is None else read_png(assets/f'shoot-busy-{phase:02d}.png')[2]
            v.stamp(contact,pw,col*(w+8)+4,row*(h+8)+4,rgba,w,h)
    save_rgb(assets/'sweep-contact-2x.png',contact,pw,ph,2)
