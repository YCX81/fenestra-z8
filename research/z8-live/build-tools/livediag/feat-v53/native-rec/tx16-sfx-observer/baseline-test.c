#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MEMTX_OK 0
#define MEMTXATTRS_UNSPECIFIED 0
#define QEMU_CLOCK_VIRTUAL 0
static void cleanup_alloc(void*p){free(*(void**)p);}
#define g_autofree __attribute__((cleanup(cleanup_alloc)))
#define g_malloc malloc
#define Z8_AUDIO_TX_BUDGET (16u*1024u*1024u)
typedef uint64_t hwaddr;
typedef struct { bool pending;uint64_t due;} Timer;
typedef struct {size_t len;uint8_t data[192000*12];} ByteArray;
typedef struct {bool serial5_tx_drain_model,bootstrap_fixtures,bus_window_fixture,serial_fixture,serial5_tx16_ring_model;char*serial5_audio_output_id;uint32_t serial5_config[5][8],peripheral24[24][7],peripheral_global[3];unsigned serial5_tx_drain_phase;bool serial5_tx_drain_prefilled;Timer*serial5_tx_drain_timer;ByteArray*serial5_pcm_capture;void*serial5_tx_drain_hash;uint64_t serial5_tx_drain_bytes,serial5_tx_drain_jobs,serial5_tx_drain_last_source,serial5_tx_drain_empty_reads;const char*serial5_tx_drain_fault;} NikonZ8State;
static int address_space_memory;static uint8_t ram[960000];static ByteArray capture;static Timer timer;static uint64_t now;static unsigned irqs;static bool readfail;
static bool pdma_ram_address(NikonZ8State*s,unsigned d,uint32_t a,unsigned n,uint64_t*out){if(d!=0||a<0xc7000000||n>sizeof(ram)||a-0xc7000000>sizeof(ram)-n)return false;*out=0xa17000000ull+a-0xc7000000;return true;}
static int address_space_read(void*a,uint64_t p,int attrs,void*out,unsigned n){if(readfail||p<0xa17000000ull||n>sizeof(ram)||p-0xa17000000ull>sizeof(ram)-n)return 1;memcpy(out,ram+(p-0xa17000000ull),n);return 0;}
static bool serial5_host_audio_enqueue(NikonZ8State*s,const uint8_t*d,unsigned n){return s->serial5_audio_output_id==NULL;}
static ByteArray*g_byte_array_new(void){capture.len=0;return &capture;}
static void g_byte_array_append(ByteArray*b,const uint8_t*d,unsigned n){assert(n<=sizeof(b->data)-b->len);memcpy(b->data+b->len,d,n);b->len+=n;}
static void g_checksum_update(void*a,const uint8_t*d,unsigned n){}
static void pdma_progress(NikonZ8State*s,unsigned ch,unsigned left){assert(!left);assert(capture.len==s->serial5_tx_drain_bytes);s->peripheral24[ch][0]&=~0x80000000u;s->peripheral24[ch][1]|=0x10000;irqs++;}
static void timer_del(Timer*t){t->pending=0;}static bool timer_pending(Timer*t){return t->pending;}static void timer_mod(Timer*t,uint64_t due){t->pending=1;t->due=due;}static uint64_t qemu_clock_get_ns(int clk){return now;}
static void serial5_tx_drain_schedule(NikonZ8State*s);
static int serial5_tx_drain_request(NikonZ8State *s, uint64_t *physical,
                                   unsigned *bytes)
{
    if (!s->serial5_tx_drain_model || !s->bootstrap_fixtures ||
        !s->bus_window_fixture || !s->serial_fixture ||
        !(s->serial5_config[0][0] & 0x2000)) {
        return -1;
    }
    bool paired=(s->serial5_config[0][0]&2)!=0;
    /* Private original F REC contract: packed stereo LE16, two request
     * banks27/28 to FIFO104, exactly192000B each. Not a generic sink. */
    bool narrow16=s->serial5_tx16_ring_model;
    if (narrow16 && (paired || s->serial5_audio_output_id)) { return -1; }
    int lanes[2]={-1,-1};uint64_t addresses[2]={0};unsigned lengths[2]={0};
    for (unsigned ch=0;ch<24;ch++) {
        uint32_t *r=s->peripheral24[ch];
        if(!(r[0]&0x80000000) || r[6]!=(paired?0x0e20013c:0x0e200104)) { continue; }
        unsigned request=(r[0]>>24)&0x3f;
        if(request!=27 && !((paired || narrow16) && request==28)) { return -1; }
        unsigned lane=request-27,length=((r[0]&0xffff)+1)*4;uint64_t source;
        if(lanes[lane]>=0 || (narrow16 && length!=192000) || (r[0]&0x40ff0000) || (r[1]&0x70000) ||
            (r[1]&~0x001f0000u)!=0x29e00000 || (r[2]&~0x404u) || r[3] ||
            !(s->peripheral_global[ch/8]&0x80000000) || (r[4]&3) ||
            !pdma_ram_address(s,0,r[4],length,&source)) { return -1; }
        lanes[lane]=ch;addresses[lane]=source;lengths[lane]=length;
    }
    if (narrow16 && lanes[0]>=0 && lanes[1]>=0 &&
        addresses[0]<addresses[1]+lengths[1] && addresses[1]<addresses[0]+lengths[0]) { return -1; }
    unsigned lane=(paired || narrow16)?s->serial5_tx_drain_phase:0;
    if(lanes[lane]<0) { return -1; }
    *physical=addresses[lane];*bytes=lengths[lane];return lanes[lane];
}
static void serial5_tx_drain_tick(void *opaque)
{
    NikonZ8State *s = opaque;
    uint64_t source = 0;
    unsigned bytes = 0;
    int channel = serial5_tx_drain_request(s, &source, &bytes);
    if (channel < 0 || s->serial5_tx_drain_fault) {
        return;
    }
    if (bytes > Z8_AUDIO_TX_BUDGET - s->serial5_tx_drain_bytes ||
        s->serial5_tx_drain_jobs >= 1024) {
        s->serial5_tx_drain_fault = "bounded-tx-budget-exhausted";
        return;
    }
    g_autofree uint8_t *data = g_malloc(bytes);
    if (address_space_read(&address_space_memory, source, MEMTXATTRS_UNSPECIFIED,
                           data, bytes) != MEMTX_OK) {
        s->serial5_tx_drain_fault = "mapped-source-read-failed";
        return;
    }
    if(!serial5_host_audio_enqueue(s,data,bytes)) { s->serial5_tx_drain_fault="host-audio-output-unavailable";return; }
    if(!s->serial5_pcm_capture) { s->serial5_pcm_capture=g_byte_array_new(); }
    g_byte_array_append(s->serial5_pcm_capture,data,bytes);
    g_checksum_update(s->serial5_tx_drain_hash, data, bytes);
    s->serial5_tx_drain_bytes += bytes;
    s->serial5_tx_drain_jobs++;
    s->serial5_tx_drain_last_source = source;
    bool paired=(s->serial5_config[0][0]&2)!=0;
    pdma_progress(s, channel, 0);
    if(paired || s->serial5_tx16_ring_model) { s->serial5_tx_drain_phase^=1;s->serial5_tx_drain_prefilled=true; }
    /* A second descriptor, if any, is a separate transfer and deadline. */
    serial5_tx_drain_schedule(s);
}
static void serial5_tx_drain_schedule(NikonZ8State *s)
{
    if (!s->serial5_tx_drain_timer) {
        return;
    }
    uint64_t source = 0;
    unsigned bytes = 0;
    if(!(s->serial5_config[0][0]&0x2000)) { s->serial5_tx_drain_phase=0;s->serial5_tx_drain_prefilled=false; }
    if (s->serial5_tx_drain_fault ||
        serial5_tx_drain_request(s, &source, &bytes) < 0) {
        timer_del(s->serial5_tx_drain_timer);
    } else if (!timer_pending(s->serial5_tx_drain_timer)) {
        /* Declared functional stereo LE32/48k clock with one prefilled
         * block. This is neither measured FIFO depth nor silicon timing. */
        timer_mod(s->serial5_tx_drain_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (((s->serial5_config[0][0]&2) || s->serial5_tx16_ring_model) && s->serial5_tx_drain_prefilled ?
                   MAX(UINT64_C(1000000),(uint64_t)bytes*1000000000/(48000*(s->serial5_tx16_ring_model?4:8))) : 1000000));
    }
}
static bool serial5_tx_drain_read(NikonZ8State *s, hwaddr addr,
                                  uint64_t *value, unsigned size)
{
    if (!s->serial5_tx_drain_model || !s->serial5_tx_drain_jobs ||
        s->serial5_tx_drain_fault || size != 4 || addr != 0x0e20011c) {
        return false;
    }
    /* Only the empty FIFO after a consumed descriptor is covered. An armed
     * replacement, including an unsupported descriptor, is not empty. */
    for (unsigned ch = 0; ch < 24; ch++) {
        if ((s->peripheral24[ch][0] & 0x80000000) &&
            (s->peripheral24[ch][6] == 0x0e200104 || s->peripheral24[ch][6] == 0x0e20013c)) {
            return false;
        }
    }
    *value = 0;
    s->serial5_tx_drain_empty_reads++;
    return true;
}
static NikonZ8State s;
static void arm(unsigned ch,unsigned req,unsigned block){uint32_t*r=s.peripheral24[ch];memset(r,0,28);r[0]=0x80000000u|(req<<24)|47999;r[1]=0x29e80000;r[2]=0x404;r[4]=0xc7000000+192000*block;r[6]=0x0e200104;}
static void setup(void){memset(&s,0,sizeof(s));memset(&timer,0,sizeof(timer));capture.len=0;now=0;irqs=0;readfail=0;s.serial5_tx_drain_model=s.bootstrap_fixtures=s.bus_window_fixture=s.serial_fixture=1;s.serial5_config[0][0]=0x450a2000;s.peripheral_global[1]=0x80000000;s.serial5_tx_drain_timer=&timer;arm(12,27,0);arm(13,28,1);for(unsigned i=0;i<sizeof(ram);i++)ram[i]=(i*13+i/65536)&255;}
int main(void){unsigned cases=0;uint64_t source;unsigned bytes;setup();assert(serial5_tx_drain_request(&s,&source,&bytes)==-1);cases++;s.serial5_tx16_ring_model=1;assert(serial5_tx_drain_request(&s,&source,&bytes)==12&&bytes==192000);cases++;
 for(unsigned bad=0;bad<22;bad++){
  setup();s.serial5_tx16_ring_model=1;
  switch(bad){case 0:s.serial_fixture=0;break;case 1:s.bus_window_fixture=0;break;case 2:s.bootstrap_fixtures=0;break;case 3:s.serial5_tx_drain_model=0;break;case 4:s.serial5_config[0][0]&=~0x2000;break;case 5:s.serial5_config[0][0]|=2;break;case 6:s.serial5_audio_output_id="dummy";break;case 7:s.peripheral24[12][0]=(27<<24)|0x80000000;break;case 8:s.peripheral24[12][0]^=0x10000;break;case 9:s.peripheral24[12][0]^=3u<<24;break;case 10:s.peripheral24[12][1]|=0x10000;break;case 11:s.peripheral24[12][1]^=0x200000;break;case 12:s.peripheral24[12][2]|=0x10;break;case 13:s.peripheral24[12][3]=1;break;case 14:s.peripheral_global[1]=0;break;case 15:s.peripheral24[12][4]++;break;case 16:s.peripheral24[12][4]=0x100000;break;case 17:s.peripheral24[12][6]=0x0e20013c;break;case 18:arm(14,27,2);break;case 19:s.peripheral24[13][4]=s.peripheral24[12][4]+4;break;case 20:s.peripheral24[13][0]&=~0x80000000u;s.serial5_tx_drain_phase=1;break;case 21:s.peripheral24[12][0]=(s.peripheral24[12][0]&0xffff0000)|0xffff;break;}
  if(serial5_tx_drain_request(&s,&source,&bytes)!=-1)fprintf(stderr,"bad case %u accepted\n",bad);assert(serial5_tx_drain_request(&s,&source,&bytes)==-1);serial5_tx_drain_schedule(&s);assert(!timer.pending);cases++;
 }
 setup();s.serial5_tx16_ring_model=1;serial5_tx_drain_schedule(&s);assert(timer.due==1000000);
 for(unsigned i=0;i<8;i++){
  assert(timer.pending);now=timer.due;timer.pending=0;serial5_tx_drain_tick(&s);assert(s.serial5_tx_drain_jobs==i+1&&irqs==i+1&&s.serial5_tx_drain_phase==((i+1)&1));assert(!memcmp(capture.data+i*192000,ram+(i%5)*192000,192000));unsigned ch=12+(i&1);arm(ch,27+(i&1),(i+2)%5);serial5_tx_drain_schedule(&s);assert(timer.due==now+1000000000);cases++;
 }
 uint64_t old=s.serial5_tx_drain_bytes;s.serial5_config[0][0]&=~0x2000;serial5_tx_drain_schedule(&s);assert(!timer.pending);serial5_tx_drain_tick(&s);assert(s.serial5_tx_drain_bytes==old);cases++;
 setup();s.serial5_tx16_ring_model=1;readfail=1;serial5_tx_drain_tick(&s);assert(s.serial5_tx_drain_fault&&irqs==0&&s.serial5_tx_drain_bytes==0);cases++;
 setup();s.serial5_tx16_ring_model=1;s.serial5_tx_drain_bytes=Z8_AUDIO_TX_BUDGET;serial5_tx_drain_tick(&s);assert(s.serial5_tx_drain_fault&&irqs==0);cases++;
 printf("{\"passed\":true,\"cases\":%u,\"clock\":\"explicit functional48k stereoLE16,first1ms then1s per192000B\",\"scope\":\"actual request/tick/schedule C; RAM/PDMA/IRQ leaves instrumented; no acoustic claim\"}\n",cases);
}
