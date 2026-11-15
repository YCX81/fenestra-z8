#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <inttypes.h>
typedef uint64_t hwaddr;
#define MEMTX_OK 0
#define MEMTXATTRS_UNSPECIFIED 0
static int address_space_memory;
static uint16_t lduw_le_p(const void*p){uint16_t v;memcpy(&v,p,2);return v;}
static uint32_t ldl_le_p(const void*p){uint32_t v;memcpy(&v,p,4);return v;}
static uint64_t ldq_le_p(const void*p){uint64_t v;memcpy(&v,p,8);return v;}
static void w32(void*p,uint32_t v){memcpy(p,&v,4);}static void w64(void*p,uint64_t v){memcpy(p,&v,8);}
typedef struct {bool active;uint16_t tx[64],rx[64];unsigned tx_head,tx_queued,rx_head,rx_queued,word_bits;uint32_t conditions,pending,start;uint64_t written,clocked,read;} Z8SerialPIO;
typedef struct {bool linked;} Transfer;
typedef Transfer Z8SerialTransfer;
typedef struct {bool codec_peer_enabled,codec_peer_cs;unsigned codec_peer_words,codec_peer_pending,codec_peer_valid;uint16_t codec_peer_value[2],codec_peer_packet[2];uint64_t codec_peer_cs_addr,codec_peer_writes,codec_peer_reads,codec_peer_rejects;uint8_t codec_peer_last_descriptor[0x68];unsigned codec_peer_last_control,codec_peer_last_queued,codec_peer_last_tx;const char*codec_peer_last_fault;Z8SerialPIO serial_pio[11];Transfer serial_transfer[11];uint32_t serial_config[11][10];} NikonZ8State;
static uint8_t glob[40],gpio[16],handle[16],table[8],desc[0x68],source[4],dest[2];
struct map {uint64_t a;size_t n;uint8_t*p;} maps[]={ {0xa02d4c908,40,glob},{0xa028097d8,16,gpio},{0xa02813f00,16,handle},{0xa02c96da8,8,table},{0xa02808b90,0x68,desc},{0xa10000000,4,source},{0xa10000100,2,dest} };
static void *ram(uint64_t a,size_t n){for(unsigned i=0;i<sizeof(maps)/sizeof(*maps);i++)if(a>=maps[i].a&&n<=maps[i].n&&a-maps[i].a<=maps[i].n-n)return maps[i].p+a-maps[i].a;return NULL;}
static bool dma_ram_range(NikonZ8State*s,uint64_t a,size_t n){return ram(a,n)!=NULL;}
static int address_space_read(void*a,uint64_t b,int attrs,void*p,size_t n){void*q=ram(b,n);if(!q)return 1;memcpy(p,q,n);return 0;}
static void serial_pio_update(NikonZ8State*s,unsigned c){Z8SerialPIO*p=&s->serial_pio[c];unsigned rt=s->serial_config[c][8]&255,tt=(s->serial_config[c][8]>>8)&255;uint32_t v=0;if(p->active)v=(!p->tx_queued?1:0)|(p->tx_queued<=tt?2:0)|(p->rx_queued&&p->rx_queued>=(rt?rt:1)?4:0);p->pending|=v&~p->conditions;p->conditions=v;}
/* Private exact-F functional input, not a codec identity/reset model.
 * Only wire commands 0015/0016 + value and 8015/8016 + one RX word.
 * All addresses below are ordinary guest RAM except the GPIO address read
 * from the original RAM table. No guest memory/status/return is written. */
static bool codec_peer_ram(NikonZ8State *s, uint64_t a, void *p, size_t n)
{
    return dma_ram_range(s,a,n) && address_space_read(&address_space_memory,a,
        MEMTXATTRS_UNSPECIFIED,p,n)==MEMTX_OK;
}

static bool codec_peer_identity(NikonZ8State *s, uint64_t *gpio)
{
    uint8_t g[40], o[16], h[16], a[8];
    if (!codec_peer_ram(s,UINT64_C(0xa02d4c908),g,sizeof(g)) ||
        ldq_le_p(g)!=UINT64_C(0xa028097d8) || g[8]!=1 || g[25]!=0 ||
        ldl_le_p(g+28)!=10 ||
        !codec_peer_ram(s,ldq_le_p(g),o,sizeof(o)) ||
        ldl_le_p(o+4)!=5 || ldl_le_p(o+8)!=13 || o[12]!=1 ||
        !codec_peer_ram(s,UINT64_C(0xa02813f00),h,sizeof(h)) ||
        ldq_le_p(h+8)!=UINT64_C(0xa02808b90) ||
        !codec_peer_ram(s,UINT64_C(0xa02c96da8),a,sizeof(a))) { return false; }
    *gpio=ldq_le_p(a);
    return *gpio>=0x0d150000 && *gpio<0x0d151080 && !(*gpio&3);
}

static bool codec_peer_descriptor(NikonZ8State *s, uint8_t *d)
{
    uint64_t gpio;
    return codec_peer_identity(s,&gpio) && s->codec_peer_cs &&
        codec_peer_ram(s,UINT64_C(0xa02808b90),d,0x68) &&
        ldl_le_p(d+8)==10 && ldl_le_p(d+0x40)==1 &&
        !d[0x54] && s->serial_pio[10].active &&
        s->serial_pio[10].word_bits==16 && !s->serial_transfer[10].linked &&
        (s->serial_config[10][7]&1);
}

static void codec_peer_gpio(NikonZ8State *s, hwaddr addr, uint64_t value, unsigned size)
{
    uint64_t gpio;
    if (!s->codec_peer_enabled || size!=4 || !(value&UINT64_C(0x20000000)) ||
        !codec_peer_identity(s,&gpio) || addr!=gpio) { return; }
    bool selected=!(value&0x2000);
    if (!selected && s->codec_peer_cs && s->codec_peer_words==2 &&
        (s->codec_peer_packet[0]==0x15 || s->codec_peer_packet[0]==0x16)) {
        unsigned i=s->codec_peer_packet[0]-0x15;
        s->codec_peer_value[i]=s->codec_peer_packet[1];
        s->codec_peer_valid|=1u<<i;s->codec_peer_writes++;
        fprintf(stderr,"Z8_CODEC_PEER write reg=%x value=%x cs=%"PRIx64"\n",
            i+0x15,s->codec_peer_value[i],gpio);
    }
    /* Every explicit CS edge terminates the previous packet. In particular,
     * no stale read command is allowed across CS deassert/assert. */
    s->codec_peer_cs=selected;s->codec_peer_words=0;
    s->codec_peer_pending=0;s->codec_peer_cs_addr=gpio;
}

static void codec_peer_receive(NikonZ8State *s)
{
    uint8_t d[0x68]={0};Z8SerialPIO *p=&s->serial_pio[10];
    if (!s->codec_peer_enabled || !s->codec_peer_pending) { return; }
    codec_peer_ram(s,UINT64_C(0xa02808b90),d,sizeof(d));
    memcpy(s->codec_peer_last_descriptor,d,sizeof(d));
    s->codec_peer_last_control=s->serial_config[10][1];
    s->codec_peer_last_queued=p->tx_queued;
    s->codec_peer_last_tx=p->tx_queued?p->tx[p->tx_head]:0;
    const char *fault=NULL;
    bool dummy=!!(s->serial_config[10][1]&0x20);
    if (!codec_peer_descriptor(s,d)) { fault="identity-or-controller"; }
    else if (ldq_le_p(d+0x20) || ldl_le_p(d+0x28)!=(unsigned)dummy ||
        !ldq_le_p(d+0x30) || (ldq_le_p(d+0x30)&1) ||
        !dma_ram_range(s,ldq_le_p(d+0x30),2) || ldl_le_p(d+0x38)!=1 ||
        ldl_le_p(d+0x3c) || p->rx_queued || p->clocked) { fault="not-native-rx-one-word"; }
    else if (s->codec_peer_reads>=1024) { fault="read-budget"; }
    else if (!(s->codec_peer_valid&(1u<<(s->codec_peer_pending-1)))) { fault="no-written-value"; }
    else if (dummy && !p->tx_queued) {
        /* The original40c15224 has not supplied its clock word yet. */
        s->codec_peer_last_fault="await-native-dummy-zero";return;
    } else if (p->tx_queued!=(unsigned)dummy || (dummy && p->tx[p->tx_head])) {
        fault="not-single-native-dummy-zero";
    }
    if (fault) {
        s->codec_peer_rejects++;s->codec_peer_last_fault=fault;
        fprintf(stderr,"Z8_CODEC_PEER rejected=%s control_c=%x tx=%"PRIx64
            " nt=%u rx=%"PRIx64" nr=%u queued=%u clocked=%"PRIu64"\n",
            fault,s->serial_config[10][1],ldq_le_p(d+0x20),ldl_le_p(d+0x28),
            ldq_le_p(d+0x30),ldl_le_p(d+0x38),p->tx_queued,p->clocked);return;
    }
    unsigned i=s->codec_peer_pending-1;
    if (dummy) { p->tx_head=(p->tx_head+1)%64;p->tx_queued--; }
    p->rx[p->rx_head]=s->codec_peer_value[i];p->rx_queued=1;p->clocked++;
    s->codec_peer_pending=0;s->codec_peer_reads++;s->codec_peer_last_fault="accepted";
    serial_pio_update(s,10);
    fprintf(stderr,"Z8_CODEC_PEER read reg=%x value=%x gpio=%"PRIx64
        " descriptor=a02808b90 rx=%"PRIx64" tx=0 nt=%u dummy=%u count=1 pending=%x\n",
        i+0x15,s->codec_peer_value[i],s->codec_peer_cs_addr,ldq_le_p(d+0x30),
        ldl_le_p(d+0x28),dummy,p->pending);
}

static void codec_peer_transmit(NikonZ8State *s, unsigned ch)
{
    uint8_t d[0x68], source[4];Z8SerialPIO *p=&s->serial_pio[10];
    if (s->codec_peer_enabled && ch==10 && s->codec_peer_pending &&
        codec_peer_ram(s,UINT64_C(0xa02808b90),d,sizeof(d)) &&
        !ldq_le_p(d+0x20) && ldq_le_p(d+0x30)) {
        codec_peer_receive(s);return;
    }
    if (!s->codec_peer_enabled || ch!=10 || !p->tx_queued ||
        !codec_peer_descriptor(s,d) || ldq_le_p(d+0x30) || ldl_le_p(d+0x38) ||
        !ldq_le_p(d+0x20) || (ldq_le_p(d+0x20)&1)) { return; }
    unsigned n=ldl_le_p(d+0x28);
    if ((n!=1 && n!=2) || !codec_peer_ram(s,ldq_le_p(d+0x20),source,n*2)) { return; }
    unsigned first=lduw_le_p(source);
    if (!((n==2 && (first==0x15 || first==0x16)) ||
          (n==1 && (first==0x8015 || first==0x8016)))) { return; }
    if (s->codec_peer_writes>=1024 || s->codec_peer_words>=n ||
        p->tx_queued>n-s->codec_peer_words || p->clocked!=s->codec_peer_words) {
        s->codec_peer_rejects++;return;
    }
    /* Validate the whole queued batch before producing any clocks. */
    for (unsigned k=0;k<p->tx_queued;k++) {
        if (p->tx[(p->tx_head+k)%64]!=lduw_le_p(source+2*(s->codec_peer_words+k))) {
            s->codec_peer_rejects++;return;
        }
    }
    while (p->tx_queued) {
        s->codec_peer_packet[s->codec_peer_words++]=p->tx[p->tx_head];
        p->tx_head=(p->tx_head+1)%64;p->tx_queued--;p->clocked++;
    }
    if (n==1) { s->codec_peer_pending=first-0x8015+1; }
    serial_pio_update(s,10);
}

static bool actual_serial_enable(NikonZ8State *s, unsigned ch)
{
    Z8SerialTransfer next = { 0 };
    uint32_t fifo = 0x0e700000 + ch * 0x100;
    int tx = -1, rx = -1;
    /* Original DMA entry 40c145f4 sets +c bit10; PIO entries 40c147c8
     * and 40c1482c clear it. +10 bit3 is NOT the PIO/DMA selector. */
    if (!(s->serial_config[ch][1] & 0x400)) {
        Z8SerialPIO *p = &s->serial_pio[ch];
        unsigned bits = ((s->serial_config[ch][2] >> 8) & 31) + 1;
        if ((bits != 8 && bits != 9 && bits != 16) ||
            p->active || s->serial_transfer[ch].linked) {
            return false;
        }
        memset(p, 0, sizeof(*p));
        p->active = true;
        p->word_bits = bits;
        serial_pio_update(s, ch);
        fprintf(stderr, "Z8_SERIAL PIO enabled ch=%u bits=%u mask=%x thresholds=%x; external clock required\n",
                ch, bits, s->serial_config[ch][9], s->serial_config[ch][8]);
        return true;
    }
    return false;
}

static NikonZ8State s;
static unsigned cases;
static void setup(void){memset(&s,0,sizeof(s));memset(glob,0,sizeof(glob));memset(gpio,0,sizeof(gpio));memset(handle,0,sizeof(handle));memset(desc,0,sizeof(desc));s.codec_peer_enabled=1;w64(glob,0xa028097d8);glob[8]=1;w32(glob+28,10);w32(gpio+4,5);w32(gpio+8,13);gpio[12]=1;w64(handle+8,0xa02808b90);w64(table,0x0d151014);w32(desc+8,10);w32(desc+0x40,1);s.serial_config[10][7]=1;s.serial_config[10][8]=0x2001;codec_peer_gpio(&s,0x0d151014,0x20000000,4);}
static void start(unsigned tx,unsigned rx){s.serial_pio[10].active=false;s.serial_config[10][2]=15<<8;assert(actual_serial_enable(&s,10));assert(s.serial_pio[10].clocked==0);w64(desc+0x20,tx?0xa10000000:0);w32(desc+0x28,tx);w32(desc+0x2c,0);w64(desc+0x30,rx?0xa10000100:0);w32(desc+0x38,rx);w32(desc+0x3c,0);}
static void queue(uint16_t v){Z8SerialPIO*p=&s.serial_pio[10];p->tx[(p->tx_head+p->tx_queued)%64]=v;p->tx_queued++;codec_peer_transmit(&s,10);}
static void tx(unsigned reg,unsigned val){start(2,0);uint16_t x[2]={reg,val};memcpy(source,x,4);queue(reg);queue(val);codec_peer_gpio(&s,0x0d151014,0x20002000,4);}
static void cmd(unsigned reg){codec_peer_gpio(&s,0x0d151014,0x20000000,4);start(1,0);uint16_t v=reg|0x8000;memcpy(source,&v,2);queue(v);start(0,1);}
int main(void){
 for(unsigned r=0x15;r<=0x16;r++)for(unsigned v=0;v<=0xffff;v+=0x5555){setup();tx(r,v);assert(s.codec_peer_valid==(1u<<(r-0x15)));assert(s.codec_peer_writes==1);cmd(r);codec_peer_receive(&s);assert(s.codec_peer_reads==1&&s.serial_pio[10].rx_queued==1&&s.serial_pio[10].rx[0]==v&&(s.serial_pio[10].pending&4));codec_peer_receive(&s);assert(s.codec_peer_reads==1);cases++;}
 for(unsigned fault=0;fault<25;fault++){
  setup();tx(0x15,0x1234);cmd(0x15);
  switch(fault){
   case 0:s.codec_peer_enabled=0;break;case 1:s.codec_peer_valid=0;break;
   case 2:codec_peer_gpio(&s,0x0d151014,0x20002000,4);break;
   case 3:w32(glob+28,1);break;case 4:w64(glob,0xa028097e0);break;
   case 5:glob[8]=0;break;case 6:glob[25]=1;break;case 7:w32(gpio+4,6);break;case 8:w32(gpio+8,12);break;case 9:gpio[12]=0;break;
   case 10:w64(handle+8,0xa02808b98);break;case 11:w64(table,0x15400000);break;
   case 12:w32(desc+8,1);break;case 13:w32(desc+0x40,0);break;case 14:desc[0x54]=1;break;case 15:s.serial_pio[10].active=0;break;case 16:s.serial_pio[10].word_bits=8;break;case 17:s.serial_transfer[10].linked=1;break;case 18:s.serial_config[10][7]=0;break;
   case 19:w64(desc+0x20,0xa10000000);break;case 20:w32(desc+0x38,2);break;case 21:w64(desc+0x30,0xa10000101);break;case 22:s.serial_pio[10].tx_queued=1;break;case 23:s.serial_pio[10].rx_queued=1;break;case 24:s.codec_peer_reads=1024;break;
  }
  uint64_t n=s.codec_peer_reads;codec_peer_receive(&s);assert(s.codec_peer_reads==n);cases++;
 }
 for(unsigned reg=0;reg<0x20;reg++){if(reg==0x15||reg==0x16)continue;setup();tx(reg,0x5555);assert(!s.codec_peer_valid);cmd(reg);codec_peer_receive(&s);assert(!s.codec_peer_reads);cases++;}
 setup();tx(0x15,0x1234);cmd(0x15);codec_peer_gpio(&s,0x0d151014,0x20002000,4);codec_peer_gpio(&s,0x0d151014,0x20000000,4);codec_peer_receive(&s);assert(!s.codec_peer_reads);cases++;
 setup();start(2,0);uint16_t a[2]={0x15,0x1234};memcpy(source,a,4);queue(0x15);queue(0x9999);codec_peer_gpio(&s,0x0d151014,0x20002000,4);assert(!s.codec_peer_valid);cases++;
 for(unsigned fault=0;fault<6;fault++){
  setup();tx(0x15,0x1234);cmd(0x15);s.serial_config[10][1]|=0x20;w32(desc+0x28,1);
  codec_peer_receive(&s);assert(!s.codec_peer_reads && !strcmp(s.codec_peer_last_fault,"await-native-dummy-zero"));
  if(fault==1)s.serial_config[10][1]=0;
  if(fault==2)w32(desc+0x28,0);
  if(fault==3)w64(desc+0x20,0xa10000000);
  if(fault==4)s.codec_peer_valid=0;
  queue(fault==5?1:0);
  if(fault==0){assert(s.codec_peer_reads==1 && s.serial_pio[10].tx_queued==0 && s.serial_pio[10].rx_queued==1 && s.serial_pio[10].clocked==1);}
  else assert(!s.codec_peer_reads);
  cases++;
 }
 printf("{\"cases\":%u,\"pass\":true,\"scope\":\"actual private peer C with synthetic RAM/controller input; no physical codec claim\"}\n",cases);
}
