/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Small bounded JPEG thumbnail decoder. This is not used for RAW samples.
   Integer separable IDCT (Q13 cosine coefficients); nearest chroma upsampling.
   All file, segment, entropy, Huffman, component and output bounds are checked. */
#include "thumbnail.h"
typedef struct {uint16_t count[16],start[16];uint32_t code[16];uint8_t symbol[256],valid;} tj_huff;
typedef struct {uint8_t id,h,v,qt,dc,ac;int32_t predictor;} tj_component;
typedef struct {
    const uint8_t *data;uint32_t bytes,pos,bits,held,error;
    uint16_t quant[4][64];uint8_t quant_valid[4];
    tj_huff huff[2][4];tj_component comp[3];
    uint8_t plane[3][256];
    uint32_t width,height,maxh,maxv,restart;
    int32_t coeff[64];int64_t temporary[64];
} tj_state;
static const uint8_t tj_zigzag[64]={0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,
 35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63};
static const int16_t tj_cosine[8][8]={
 {5793,8035,7568,6811,5793,4551,3135,1598},
 {5793,6811,3135,-1598,-5793,-8035,-7568,-4551},
 {5793,4551,-3135,-8035,-5793,1598,7568,6811},
 {5793,1598,-7568,-4551,5793,6811,-3135,-8035},
 {5793,-1598,-7568,4551,5793,-6811,-3135,8035},
 {5793,-4551,-3135,8035,-5793,-1598,7568,-6811},
 {5793,-6811,3135,1598,-5793,8035,-7568,4551},
 {5793,-8035,7568,-6811,5793,-4551,3135,-1598}
};
static void tj_zero(void *p,size_t n){uint8_t *b=p;while(n--)*b++=0;}
static uint32_t tj_be(const uint8_t *p){return ((uint32_t)p[0]<<8)|p[1];}
static uint8_t tj_clip(int64_t x){return x<0?0:x>255?255:(uint8_t)x;}
static uint32_t tj_bits(tj_state *s,unsigned n){
    while(s->held<n){
        if(s->pos>=s->bytes){s->error=1;return 0;}
        uint8_t b=s->data[s->pos++];
        if(b==255){if(s->pos>=s->bytes||s->data[s->pos++]!=0){s->error=1;return 0;}}
        s->bits=(s->bits<<8)|b;s->held+=8;
    }
    s->held-=n;return (s->bits>>s->held)&((1u<<n)-1u);
}
static int tj_symbol(tj_state *s,const tj_huff *h){
    if(!h->valid){s->error=1;return 0;}
    uint32_t code=0;
    for(unsigned len=0;len<16;++len){
        code=code*2+tj_bits(s,1);if(s->error)return 0;
        if(code>=h->code[len]&&code-h->code[len]<h->count[len])return h->symbol[h->start[len]+code-h->code[len]];
    }
    s->error=1;return 0;
}
static int32_t tj_extend(tj_state *s,unsigned n){
    if(!n)return 0;uint32_t x=tj_bits(s,n);
    return x<(1u<<(n-1))?(int32_t)x-(int32_t)((1u<<n)-1u):(int32_t)x;
}
static int tj_block(tj_state *s,tj_component *c,uint8_t *plane,unsigned stride,unsigned bx,unsigned by){
    int32_t *v=s->coeff;tj_zero(v,64*sizeof(*v));
    int n=tj_symbol(s,&s->huff[0][c->dc]);if(n<0||n>11)return 1;
    c->predictor+=tj_extend(s,(unsigned)n);
    if(c->predictor<-32768||c->predictor>32767)return 1;
    v[0]=c->predictor*s->quant[c->qt][0];
    for(unsigned k=1;k<64;){
        int sym=tj_symbol(s,&s->huff[1][c->ac]);if(s->error)return 1;
        unsigned run=(unsigned)sym>>4,size=(unsigned)sym&15;
        if(!size){if(!run)break;if(run!=15||k+16>64)return 1;k+=16;continue;}
        if(size>10||k+run>=64)return 1;k+=run;
        unsigned at=tj_zigzag[k++];v[at]=tj_extend(s,size)*s->quant[c->qt][at];
    }
    if(s->error)return 1;
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){
        int64_t sum=0;for(unsigned k=0;k<8;++k)sum+=(int64_t)v[y*8+k]*tj_cosine[x][k];
        s->temporary[y*8+x]=sum;
    }
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){
        int64_t sum=0;for(unsigned k=0;k<8;++k)sum+=s->temporary[k*8+x]*tj_cosine[y][k];
        /* Explicit floor division for negative values; no signed right-shift assumption. */
        sum+=(int64_t)1<<27;sum=sum>=0?sum/((int64_t)1<<28):-((-sum+(((int64_t)1<<28)-1))/((int64_t)1<<28));
        plane[(by+y)*stride+bx+x]=tj_clip(sum+128);
    }
    return 0;
}
size_t ps8_jpeg_workspace_bytes(void){return sizeof(tj_state);}
int ps8_jpeg_decode(const uint8_t *data,uint32_t bytes,uint8_t *rgba,void *workspace,size_t workspace_bytes){
    if(!data||!rgba||!workspace||workspace_bytes<sizeof(tj_state)||bytes<4||bytes>PS8_THUMB_JPEG_MAX||data[0]!=255||data[1]!=216)return 1;
    tj_state *s=workspace;tj_zero(s,sizeof(*s));s->data=data;s->bytes=bytes;s->pos=2;
    int have_frame=0,have_scan=0;
    while(s->pos+4<=bytes){
        if(data[s->pos++]!=255)return 1;while(s->pos<bytes&&data[s->pos]==255)++s->pos;
        if(s->pos+3>bytes)return 1;unsigned marker=data[s->pos++];uint32_t len=tj_be(data+s->pos);
        if(len<2||len>bytes-s->pos)return 1;
        uint32_t p=s->pos+2,end=s->pos+len;s->pos=end;
        if(marker==0xdb){
            while(p<end){unsigned id=data[p++];if(id>3||end-p<64)return 1;
                for(unsigned k=0;k<64;++k){uint8_t q=data[p++];if(!q)return 1;s->quant[id][tj_zigzag[k]]=q;}s->quant_valid[id]=1;}
        }else if(marker==0xc4){
            while(p<end){unsigned id=data[p++];if((id&15)>3||(id>>4)>1||end-p<16)return 1;
                tj_huff *h=&s->huff[id>>4][id&15];unsigned total=0,code=0;
                for(unsigned k=0;k<16;++k){unsigned n=data[p++];h->count[k]=(uint16_t)n;h->start[k]=(uint16_t)total;h->code[k]=code;
                    if(code+n>(1u<<(k+1)))return 1;total+=n;code=(code+n)*2;}
                if(!total||total>256||end-p<total)return 1;
                for(unsigned k=0;k<total;++k)h->symbol[k]=data[p++];h->valid=1;
            }
        }else if(marker==0xc0){
            if(have_frame||len!=17||data[p]!=8||data[p+5]!=3)return 1;
            s->height=tj_be(data+p+1);s->width=tj_be(data+p+3);p+=6;
            if(s->width!=PS8_THUMB_W||s->height!=PS8_THUMB_H)return 1;
            for(unsigned i=0;i<3;++i){tj_component *c=&s->comp[i];c->id=data[p++];unsigned hv=data[p++];c->qt=data[p++];
                c->h=hv>>4;c->v=hv&15;if(c->id!=i+1||!c->h||!c->v||c->h>2||c->v>2||c->qt>3)return 1;
                if(c->h>s->maxh)s->maxh=c->h;if(c->v>s->maxv)s->maxv=c->v;}
            if(s->comp[1].h!=1||s->comp[1].v!=1||s->comp[2].h!=1||s->comp[2].v!=1)return 1;
            have_frame=1;
        }else if(marker==0xdd){if(len!=4)return 1;s->restart=tj_be(data+p);
        }else if(marker==0xda){
            if(!have_frame||len!=12||data[p++]!=3)return 1;
            for(unsigned i=0;i<3;++i){if(data[p++]!=s->comp[i].id)return 1;unsigned h=data[p++];
                s->comp[i].dc=h>>4;s->comp[i].ac=h&15;if((h>>4)>3||(h&15)>3||!s->quant_valid[s->comp[i].qt])return 1;}
            if(data[p]!=0||data[p+1]!=63||data[p+2]!=0)return 1;have_scan=1;break;
        }else if((marker>=0xc0&&marker<=0xcf)||marker==0xd8||marker==0xd9)return 1;
    }
    if(!have_scan)return 1;
    unsigned mw=s->maxh*8,mh=s->maxv*8,count=0,restart_number=0;
    for(unsigned my=0;my<s->height;my+=mh)for(unsigned mx=0;mx<s->width;mx+=mw){
        if(count&&s->restart&&count%s->restart==0){
            s->held=0;s->bits=0;if(s->pos+2>bytes||data[s->pos++]!=255||data[s->pos++]!=(0xd0+(restart_number++&7)))return 1;
            for(unsigned i=0;i<3;++i)s->comp[i].predictor=0;
        }
        for(unsigned i=0;i<3;++i){tj_component *c=&s->comp[i];
            for(unsigned y=0;y<c->v;++y)for(unsigned x=0;x<c->h;++x)if(tj_block(s,c,s->plane[i],c->h*8,x*8,y*8))return 1;}
        for(unsigned y=0;y<mh&&my+y<s->height;++y)for(unsigned x=0;x<mw&&mx+x<s->width;++x){
            int32_t value[3];for(unsigned i=0;i<3;++i){tj_component *c=&s->comp[i];value[i]=s->plane[i][(y*c->v/s->maxv)*(c->h*8)+x*c->h/s->maxh];}
            int32_t yy=value[0],cb=value[1]-128,cr=value[2]-128;
            uint8_t *p=rgba+((my+y)*s->width+mx+x)*4;
            p[0]=tj_clip(yy+(91881*cr+32768)/65536);p[1]=tj_clip(yy-(22554*cb+46802*cr+32768)/65536);
            p[2]=tj_clip(yy+(116130*cb+32768)/65536);p[3]=255;
        }
        ++count;
    }
    if(s->error||s->pos+2>bytes||data[s->pos]!=255||data[s->pos+1]!=217)return 1;
    return 0;
}
static uint32_t tj_u16(const uint8_t *p,int le){return le?p[0]+((uint32_t)p[1]<<8):tj_be(p);}
static uint32_t tj_u32(const uint8_t *p,int le){return le?tj_u16(p,1)+(tj_u16(p+2,1)<<16):(tj_u16(p,0)<<16)+tj_u16(p+2,0);}
int ps8_thumbnail_read(const psm_platform *p,const char *path,uint8_t *jpeg,uint32_t *jpeg_bytes,uint8_t *scratch){
    uint32_t bytes=0;int fd=p->open(p->context,path,&bytes),status=1;
    if(fd<0)return 1;
    if(bytes<8||p->read_at(p->context,fd,0,scratch,8))goto done;
    int le=scratch[0]=='I'&&scratch[1]=='I';
    if((!le&&(scratch[0]!='M'||scratch[1]!='M'))||tj_u16(scratch+2,le)!=42)goto done;
    uint32_t off=tj_u32(scratch+4,le),image=0,size=0;
    if(off>bytes-2||p->read_at(p->context,fd,off,scratch,2))goto done;
    uint32_t n=tj_u16(scratch,le);off+=2;
    if(n>256||off>bytes||n>(bytes-off)/12)goto done;
    for(unsigned i=0;i<n;++i){
        if(p->read_at(p->context,fd,off+12*i,scratch,12))goto done;
        uint32_t tag=tj_u16(scratch,le);
        if(tag==0x201||tag==0x202){
            if(tj_u16(scratch+2,le)!=4||tj_u32(scratch+4,le)!=1)goto done;
            if(tag==0x201)image=tj_u32(scratch+8,le);else size=tj_u32(scratch+8,le);
        }
    }
    if(!size||size>PS8_THUMB_JPEG_MAX||image>bytes||size>bytes-image)goto done;
    if(p->read_at(p->context,fd,image,jpeg,size))goto done;*jpeg_bytes=size;status=0;
done:
    if(p->close(p->context,fd))status=1;return status;
}
