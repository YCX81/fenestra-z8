"""Check NCDT 4:2:2 resampling on a source with independent chroma quadrants."""
from pathlib import Path
import subprocess
import tempfile
import json
import sys
import struct

HERE = Path(__file__).resolve().parent
WORK = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE.parents[1] / 'writer/nctg-work'
sys.path.insert(0, str(HERE.parents[2] / 'feat-mov'))
import mov_fix as m

source = r'''
#include "preview_writer.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
typedef struct {FILE *out;unsigned writes;} sink;
static int write_at(void *v,uint64_t at,const void *p,size_t n){sink*s=v;assert(at<=3u*1024u*1024u&&n<=3u*1024u*1024u-at);if(fseek(s->out,(long)at,SEEK_SET))return -1;s->writes++;return fwrite(p,1,n,s->out)==n?0:-1;}
static int frame(void *v,uint32_t index,void *p,size_t n){(void)v;assert(index==22&&n==LIVE_PREVIEW_FRAME_BYTES);uint8_t*b=p;size_t area=1024u*682u;memset(b,128,area);for(unsigned y=0;y<341;y++)for(unsigned x=0;x<512;x++){b[area+y*512+x]=y<170?96:160;b[area+area/4+y*512+x]=x<256?104:152;}return 0;}
int main(int argc,char**argv){assert(argc==2);sink s={fopen(argv[1],"wb"),0};assert(s.out);void*scratch=malloc(LIVE_PREVIEW_FRAME_BYTES);uint8_t*io=malloc(4096);assert(scratch&&io);LivePreviewWriter w={&s,frame,write_at,0};LiveNikonFacts facts={1024,682,95,30000,1001,3,1};uint64_t end=0;assert(!live_nikon_metadata_write(&w,scratch,LIVE_PREVIEW_FRAME_BYTES,io,4096,0,&end,&facts));assert(end<=3u*1024u*1024u);assert(!fclose(s.out));free(scratch);free(io);return 0;}
'''
out = HERE / 'audio-paths'
out.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='sampling-', dir=HERE) as td:
    tmp = Path(td)
    (tmp / 'main.c').write_text(source)
    subprocess.run(['clang','-O1','-g','-std=c11','-fsanitize=address,undefined','-fno-sanitize-recover=all',
                    '-Wall','-Wextra','-Werror','-I',str(WORK/'src-fix37/codec'),str(tmp/'main.c'),
                    str(WORK/'src-fix37/codec/preview_writer.c'),'-o',str(tmp/'test')],check=True)
    subprocess.run([str(tmp/'test'),str(out/'chroma-pattern.udta')],check=True)
udta=m.parse((out/'chroma-pattern.udta').read_bytes())[0]
parts=m.parse(m.direct_child(m.parse(udta.data),b'NCDT').data)
reports=[]
for name in [b'NCTH',b'NCVW',b'NCM1',b'NCM2']:
    data=m.direct_child(parts,name).data
    jpg=out/(name.decode()+'-pattern.jpg');jpg.write_bytes(data)
    sof=data.index(b'\xff\xc0');height,width=struct.unpack_from('>HH',data,sof+5)
    assert data[sof+11]==0x21
    ppm=subprocess.check_output(['djpeg','-pnm',str(jpg)],stderr=subprocess.PIPE)
    magic,dimensions,maxval,pixels=ppm.split(b'\n',3)
    assert magic==b'P6' and dimensions==f'{width} {height}'.encode() and maxval==b'255'
    assert len(pixels)==width*height*3
    if 1024*height>width*682:fw=width;fh=(682*width//1024)&~1
    else:fh=height;fw=(1024*height//682)&~1
    left=((width-fw)//2)&~1;top=((height-fh)//2)&~1
    samples=[]
    for row in range(2):
        for col in range(2):
            y=top+fh*(1+2*row)//4;x=left+fw*(1+2*col)//4
            rgb=list(pixels[(y*width+x)*3:(y*width+x)*3+3])
            u=96 if row==0 else 160;v=104 if col==0 else 152
            expected=[round(128+1.402*(v-128)),round(128-.344136*(u-128)-.714136*(v-128)),round(128+1.772*(u-128))]
            assert max(abs(a-b) for a,b in zip(rgb,expected))<=8,(name,row,col,rgb,expected)
            samples.append(dict(row=row,col=col,rgb=rgb,expected=expected))
    reports.append(dict(image=name.decode(),width=width,height=height,format='4:2:2',samples=samples))
(out/'chroma-sampling.json').write_text(json.dumps(dict(status='PASS',source=str(WORK),images=reports),indent=2)+'\n')
print('PASS: four NCDT 4:2:2 JPEGs, chroma quadrants, scaling, ASan/UBSan')
