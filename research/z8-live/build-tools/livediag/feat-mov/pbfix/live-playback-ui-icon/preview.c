#include "preview.h"
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
int lp_preview_frame(const LpPreview *p,uint64_t t,uint64_t epoch,uint64_t *off){
    if(!p||!off||epoch!=p->epoch||!p->frames||!p->duration_us||!p->frame_bytes)return -1;
    if(t>=p->duration_us)return 1; /* EOS: restore original photo through GUI owner. */
    /* t is bounded before multiply. Drop overdue frames instead of blocking GUI. */
    uint32_t i=(uint32_t)(t*p->frames/p->duration_us);
    *off=32+(uint64_t)i*p->frame_bytes;return 0;
}
static int alnum_(char c){return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_';}
int lp_sidecar_path(char out[64],const char folder[9],const char stem[9]){
    if(!out||!folder||!stem||folder[8]||stem[8])return -1;
    for(unsigned i=0;i<8;i++)if(!alnum_(folder[i])||!alnum_(stem[i]))return -1;
    for(unsigned i=0;i<3;i++)if(folder[i]<'0'||folder[i]>'9')return -1;
    unsigned d=(folder[0]-'0')*100+(folder[1]-'0')*10+folder[2]-'0';
    if(d<100||d>999)return -1;
    unsigned n=0;for(unsigned i=4;i<8;i++){if(stem[i]<'0'||stem[i]>'9')return -1;n=n*10+stem[i]-'0';}
    if(!n)return -1;
    const char *prefix="\\LIVE\\";unsigned j=0;for(;prefix[j];j++)out[j]=prefix[j];
    for(unsigned i=0;i<8;i++)out[j++]=folder[i];out[j++]='\\';
    for(unsigned i=0;i<8;i++)out[j++]=stem[i];out[j++]='\\';out[j]=0;return 0;
}
