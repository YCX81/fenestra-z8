/* v37: reader side of the embedded preview (atom parser + baseline JPEG decoder).
 * It is compiled into the Live layer, which has the room; the playback layer calls
 * these functions through link symbols (tools-v37/build.py passes their addresses).
 * Declarations: pb-fix37/preview.h (kept identical in codec/preview_reader.h). */
#include "preview_reader.h"
static uint32_t le16(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8;}
static uint32_t le32(const uint8_t *p){return le16(p)|le16(p+2)<<16;}
int lp_preview_open(const void *blob,uint64_t size,LpPreview *o,uint64_t epoch){
    if(!blob||!o||size<32)return -1;
    const uint8_t *p=blob;
    if(le32(p)!=0x3150564c||le16(p+4)!=1||le16(p+6)!=32||le32(p+28))return -1;
    uint32_t w=le16(p+8),h=le16(p+10),n=le32(p+12),dt=le32(p+16),fb=le32(p+20),bytes=le32(p+24);
    if(!w||!h||(w|h)&1||w>1024||h>682||!n||n>90||dt<33333||dt>200000)return -1;
    if(fb!=w*h*3/2||(uint64_t)fb*n!=bytes||size!=32+(uint64_t)bytes||(uint64_t)n*dt>4000000)return -1;
    *o=(LpPreview){w,h,n,fb,n*dt,epoch};return 0;
}
int lp_preview_index(const LpPreview *p,uint64_t t,uint64_t epoch,uint32_t *index){
    if(!p||!index||epoch!=p->epoch||!p->frames||!p->duration_us||!p->frame_bytes)return -1;
    if(t>=p->duration_us)return 1; /* EOS: restore original photo through GUI owner. */
    /* t is bounded before multiply. Drop overdue frames instead of blocking GUI. */
    *index=(uint32_t)(t*p->frames/p->duration_us);return 0;
}
int lp_clip_tail(const uint8_t tail[8],uint64_t file_bytes,uint64_t *atom,uint32_t *atom_bytes){
    if(!tail||!atom||!atom_bytes||tail[4]!=0x4c||tail[5]!=0x56||tail[6]!=0x4a||tail[7]!=0x54)return -1;
    uint32_t n=le32(tail);
    if(n<8u+24u+4u+8u||n>file_bytes||n>0x7fffffffu)return -1;
    *atom=file_bytes-n;*atom_bytes=n;return 0;
}
int lp_clip_open(const uint8_t *h,size_t bytes,uint64_t atom,uint32_t atom_bytes,LpPreview *o,LpClip *c,uint64_t epoch){
    if(!h||!o||!c||bytes<32u)return -1;
    uint32_t size=(uint32_t)h[0]<<24|(uint32_t)h[1]<<16|(uint32_t)h[2]<<8|h[3];
    if(size!=atom_bytes||le32(h+4)!=0x65657266u||le32(h+8)!=0x314a564cu||le16(h+12)!=1u||le32(h+28))return -1;
    uint32_t head=le16(h+14),w=le16(h+16),ht=le16(h+18),n=le32(h+20),dt=le32(h+24);
    if(!n||n>LP_CLIP_FRAMES||head!=24u+4u*n||8u+head>bytes||w!=1024u||ht!=682u||dt<33333u||dt>200000u||
       (uint64_t)n*dt>4000000u)return -1;
    uint64_t at=atom+8u+head,end=atom+atom_bytes-8u;
    for(uint32_t i=0;i<n;i++){
        uint32_t s=le32(h+32u+4u*i);
        if(s<128u||s>LP_JPEG_MAX||s>end-at)return -1;
        c->at[i]=at;at+=s;
    }
    c->at[n]=at;c->atom=atom;c->end=end;
    *o=(LpPreview){w,ht,n,w*ht*3u/2u,n*dt,epoch};return 0;
}
int lp_clip_audio(const uint8_t r[16],const LpClip *c,uint32_t *offset,uint32_t *frames){
    if(le32(r)!=0x414a564cu||le32(r+12)!=48000u)return -1;
    uint32_t at=le32(r+4),n=le32(r+8);
    if(at<32u||!n||n>48000u*8u||(uint64_t)at+(uint64_t)n*4u>c->atom)return -1;   /* inside the movie part */
    *offset=at;*frames=n;return 0;
}

/* ---- baseline JPEG ---- */
static const uint8_t zigzag[64]={0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,
    28,35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63};
static int huffman(LpHuffman *t,const uint8_t *bits,const uint8_t *vals,uint32_t count){
    uint32_t code=0,k=0;
    for(uint32_t i=0;i<512u;i++)t->look[i]=0;
    for(uint32_t len=1;len<=16u;len++){
        uint32_t n=bits[len-1u];
        t->first[len]=(uint16_t)code;t->offset[len]=(uint16_t)k;
        if(k+n>count||code+n>(1u<<len))return -1;
        for(uint32_t i=0;i<n;i++,k++,code++){
            t->vals[k]=vals[k];
            if(len<=9u)for(uint32_t f=0;f<(1u<<(9u-len));f++)t->look[(code<<(9u-len))|f]=(uint16_t)(len<<8|vals[k]);
        }
        t->maxcode[len]=n?(int32_t)code-1:-1;
        code<<=1;
    }
    return k==count?0:-1;
}
static void refill(LpJpeg *j){
    while(j->bits<=24){
        uint32_t b=0;
        if(j->p<j->end){
            b=*j->p;
            if(b==255u){
                if(j->p+1<j->end&&j->p[1]==0)j->p+=2;
                else b=0;                 /* a marker or the end: feed zeros */
            }else j->p++;
        }
        j->acc=(j->acc<<8)|b;j->bits+=8;
    }
}
static int32_t bits(LpJpeg *j,uint32_t n){
    refill(j);j->bits-=(int)n;
    return (int32_t)((j->acc>>j->bits)&((1u<<n)-1u));
}
static int32_t symbol(LpJpeg *j,const LpHuffman *t){
    refill(j);
    uint32_t e=t->look[(j->acc>>(j->bits-9))&511u];
    if(e){j->bits-=(int)(e>>8);return (int32_t)(e&255u);}
    for(uint32_t len=10;len<=16u;len++){
        int32_t code=(int32_t)((j->acc>>(j->bits-(int)len))&((1u<<len)-1u));
        if(code<=t->maxcode[len]){
            j->bits-=(int)len;
            return t->vals[(uint32_t)t->offset[len]+(uint32_t)code-t->first[len]];
        }
    }
    return -1;
}
static int32_t extend(int32_t v,uint32_t n){return v<(int32_t)(1u<<(n-1u))?v-(int32_t)((1u<<n)-1u):v;}
#define MUL(v,c) ((v)*(int32_t)(c))
static void idct_pass(int32_t *p,int step,int shift){
    int32_t z2=p[2*step],z3=p[6*step],z1=MUL(z2+z3,4433);
    int32_t t2=z1+MUL(z3,-15137),t3=z1+MUL(z2,6270);
    int32_t t0=(p[0]+p[4*step])*8192,t1=(p[0]-p[4*step])*8192;
    int32_t t10=t0+t3,t13=t0-t3,t11=t1+t2,t12=t1-t2;
    t0=p[7*step];t1=p[5*step];t2=p[3*step];t3=p[step];
    int32_t z4,z5;
    z1=t0+t3;z2=t1+t2;z3=t0+t2;z4=t1+t3;z5=MUL(z3+z4,9633);
    t0=MUL(t0,2446);t1=MUL(t1,16819);t2=MUL(t2,25172);t3=MUL(t3,12299);
    z1=MUL(z1,-7373);z2=MUL(z2,-20995);z3=MUL(z3,-16069)+z5;z4=MUL(z4,-3196)+z5;
    t0+=z1+z3;t1+=z2+z4;t2+=z2+z3;t3+=z1+z4;
    int32_t r=1<<(shift-1);
    p[0]=(t10+t3+r)>>shift;p[7*step]=(t10-t3+r)>>shift;
    p[step]=(t11+t2+r)>>shift;p[6*step]=(t11-t2+r)>>shift;
    p[2*step]=(t12+t1+r)>>shift;p[5*step]=(t12-t1+r)>>shift;
    p[3*step]=(t13+t0+r)>>shift;p[4*step]=(t13-t0+r)>>shift;
}
static uint8_t clamp8(int32_t v){return v<0?0u:v>255?255u:(uint8_t)v;}
/* One block into plane (w x h, cropped at the right and bottom edges). */
static int block(LpJpeg *j,const LpHuffman *dc,const LpHuffman *ac,const uint16_t *q,int32_t *pred,
                 uint8_t *plane,uint32_t w,uint32_t h,uint32_t x0,uint32_t y0){
    int32_t d[64];
    int32_t s=symbol(j,dc);
    if(s<0||s>11)return -1;
    if(s)*pred+=extend(bits(j,(uint32_t)s),(uint32_t)s);
    uint32_t last=0;
    for(uint32_t i=1;i<64u;i++)d[i]=0;
    d[0]=*pred*(int32_t)q[0];
    for(uint32_t k=1;k<64u;){
        s=symbol(j,ac);
        if(s<0)return -1;
        uint32_t run=(uint32_t)s>>4,n=(uint32_t)s&15u;
        if(!n){if(run!=15u)break;k+=16u;continue;}
        k+=run;if(k>63u)return -1;
        d[zigzag[k]]=extend(bits(j,n),n)*(int32_t)q[k];last=k;k++;
    }
    uint32_t cols=x0<w?(w-x0<8u?w-x0:8u):0u,rows=y0<h?(h-y0<8u?h-y0:8u):0u;
    if(!last){
        uint8_t v=clamp8(((d[0]+4)>>3)+128);
        for(uint32_t y=0;y<rows;y++){uint8_t *o=plane+(size_t)(y0+y)*w+x0;for(uint32_t x=0;x<cols;x++)o[x]=v;}
        return 0;
    }
    for(uint32_t i=0;i<8u;i++){
        int32_t *c=d+i;
        if(!(c[8]|c[16]|c[24]|c[32]|c[40]|c[48]|c[56])){int32_t v=c[0]*4;for(uint32_t r=0;r<8u;r++)c[8u*r]=v;}
        else idct_pass(c,8,11);
    }
    for(uint32_t y=0;y<rows;y++){
        idct_pass(d+8u*y,1,18);
        uint8_t *o=plane+(size_t)(y0+y)*w+x0;for(uint32_t x=0;x<cols;x++)o[x]=clamp8(d[8u*y+x]+128);
    }
    return 0;
}
int lp_jpeg_decode(LpJpeg *j,const uint8_t *data,size_t bytes,uint8_t *out,uint32_t width,uint32_t height){
    if(!j||!data||!out||bytes<4u||data[0]!=255u||data[1]!=0xd8u||(width&15u)||(height&1u))return -1;
    const uint8_t *p=data+2,*end=data+bytes;
    uint32_t have=0,tq[2]={0,0},td[2]={0,0},ta[2]={0,0};
    for(;;){
        if(end-p<4||p[0]!=255u)return -1;
        uint32_t m=p[1],n=(uint32_t)p[2]<<8|p[3];
        if(n<2u||n>(size_t)(end-p)-2u)return -1;
        const uint8_t *s=p+4,*e=p+2+n;p=e;
        if(m==0xdbu){
            while(s<e){
                uint32_t id=*s++;if(id>1u||e-s<64)return -1;
                for(uint32_t i=0;i<64u;i++)j->q[id][i]=s[i]; /* kept in zigzag order */
                s+=64;have|=1u<<id;
            }
        }else if(m==0xc0u){
            if(n!=17u||s[0]!=8u||((uint32_t)s[1]<<8|s[2])!=height||((uint32_t)s[3]<<8|s[4])!=width||s[5]!=3u||
               s[7]!=0x22u||s[10]!=0x11u||s[13]!=0x11u||s[8]>1u||s[11]>1u||s[14]!=s[11])return -1;
            tq[0]=s[8];tq[1]=s[11];have|=4u;
        }else if(m==0xc4u){
            while(s<e){
                uint32_t id=*s++,count=0;if((id&0xeeu)||e-s<16)return -1;
                for(uint32_t i=0;i<16u;i++)count+=s[i];
                if(count>256u||(size_t)(e-s)<16u+count||huffman(&j->table[id>>4][id&1u],s,s+16,count))return -1;
                s+=16u+count;have|=8u<<((id>>4)*2u+(id&1u));
            }
        }else if(m==0xdau){
            if(n!=12u||s[0]!=3u||(s[2]&0xeeu)||(s[4]&0xeeu)||s[6]!=s[4])return -1;
            td[0]=s[2]>>4;ta[0]=s[2]&1u;td[1]=s[4]>>4;ta[1]=s[4]&1u;break;
        }else if(m==0xd9u||(m>=0xc1u&&m<=0xcfu)||m==0xddu)return -1; /* no progressive, no restarts */
    }
    if(!(have&4u)||!(have&(1u<<tq[0]))||!(have&(1u<<tq[1])))return -1;
    for(uint32_t c=0;c<2u;c++)if(!(have&(8u<<td[c]))||!(have&(8u<<(2u+ta[c]))))return -1;
    j->p=p;j->end=end;j->acc=0;j->bits=0;
    uint8_t *y=out,*u=y+(size_t)width*height,*v=u+(size_t)(width/2u)*(height/2u);
    int32_t pred[3]={0,0,0};
    const LpHuffman *dy=&j->table[0][td[0]],*ay=&j->table[1][ta[0]],*dcc=&j->table[0][td[1]],*acc=&j->table[1][ta[1]];
    for(uint32_t my=0;my<height;my+=16u)for(uint32_t mx=0;mx<width;mx+=16u){
        if(block(j,dy,ay,j->q[tq[0]],&pred[0],y,width,height,mx,my)||
           block(j,dy,ay,j->q[tq[0]],&pred[0],y,width,height,mx+8u,my)||
           block(j,dy,ay,j->q[tq[0]],&pred[0],y,width,height,mx,my+8u)||
           block(j,dy,ay,j->q[tq[0]],&pred[0],y,width,height,mx+8u,my+8u)||
           block(j,dcc,acc,j->q[tq[1]],&pred[1],u,width/2u,height/2u,mx/2u,my/2u)||
           block(j,dcc,acc,j->q[tq[1]],&pred[2],v,width/2u,height/2u,mx/2u,my/2u))return -1;
    }
    return 0;
}
