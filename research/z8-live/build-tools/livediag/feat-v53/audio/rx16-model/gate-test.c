
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
typedef struct {int bootstrap_fixtures,bus_window_fixture,serial_fixture,serial5_audio_config_model;uint32_t serial5_config[5][8],peripheral24[24][8],peripheral_global[3];unsigned serial5_pcm_phase[5];uint64_t serial5_pcm_blocks[5],serial5_pcm_words[5],serial5_pcm_irqs[5];} NikonZ8State;
typedef NikonZ8State Object;typedef int Error;
#define NIKON_Z8_LAB(o) (o)
#define MEMTXATTRS_UNSPECIFIED 0
#define MEMTX_OK 0
static uint8_t ram[19200+128];static unsigned completions;static int dma_fail,address_space_memory;
static int pdma_ram_address(NikonZ8State*s,int dir,uint32_t local,unsigned n,uint64_t*out){(void)s;assert(dir==1);if(local<0x1000||local>0x1000+19200-n)return 0;*out=local;return 1;}
static int address_space_write(void*a,uint64_t p,int attr,const void*d,size_t n){(void)a;(void)attr;if(dma_fail)return 1;assert(p>=0x1000&&p+n<=0x1000+19200);memcpy(ram+64+p-0x1000,d,n);return 0;}
static void pdma_progress(NikonZ8State*s,unsigned ch,unsigned left){assert(!left);s->peripheral24[ch][0]&=~0x80000000u;completions++;}
static void error_setg(Error**p,const char*f,...){(void)f;*p=(void*)1;}
static int g_ascii_xdigit_value(char c){return c<='9'?c-'0':(c|32)-'a'+10;}
/* PRIVATE explicit packed LE16 stereo RX fixture, original F tuple only.
 * Native40c18160 chooses FIFO100 when bit0=0. 40c7e4ac ->17dac/17e5c
 * emits requests25/26,9600B banks; original ISR/refill is unchanged.
 * Each input is exactly one external50ms block, not a manufactured callback.
 * Physical serial packing/clock and microphone acoustics remain unverified. */
static int serial5_pcm16_request(NikonZ8State *s, uint64_t *physical)
{
    if (!s->bootstrap_fixtures || !s->bus_window_fixture || !s->serial_fixture ||
        !s->serial5_audio_config_model ||
        (s->serial5_config[0][0] & 0x1001) != 0x1000) { return -1; }
    int found=-1;
    for (unsigned ch=0;ch<24;ch++) {
        uint32_t *r=s->peripheral24[ch];
        unsigned request=25+s->serial5_pcm_phase[0];
        if (!(r[0]&0x80000000) || ((r[0]>>24)&0x3f)!=request ||
            r[4]!=0x0e200100) { continue; }
        unsigned words=(r[0]&0xffff)+1;
        uint64_t target;
        if (found>=0 || words!=2400 || (r[0]&0x40ff0000) || (r[1]&0x70000) ||
            (r[1]&~0x001f0000u)!=0x2ae00000 || (r[2]&~0x404u) || r[3] ||
            !(s->peripheral_global[ch/8]&0x80000000) ||
            !pdma_ram_address(s,1,r[6],9600,&target) || (target&3)) { return -1; }
        for (unsigned other=0;other<24;other++) {
            uint32_t *q=s->peripheral24[other];uint64_t address;
            unsigned bytes=((q[0]&0xffff)+1)*4;
            if (other==ch || !(q[0]&0x80000000) || q[4]<0x0e200100 ||
                q[4]>=0x0e200600 || ((q[4]&255)!=0 && (q[4]&255)!=0x38)) { continue; }
            if (pdma_ram_address(s,1,q[6],bytes,&address) &&
                target<address+bytes && address<target+9600) { return -1; }
        }
        found=ch;*physical=target;
    }
    return found;
}
static void set_serial5_pcm16_input(Object *obj,const char *value,Error **errp)
{
    NikonZ8State *s=NIKON_Z8_LAB(obj);unsigned phase;uint32_t local,words;int n=0;
    if (strspn(value,"0123456789abcdefABCDEF:")!=strlen(value) ||
        sscanf(value,"%1u:%8"SCNx32":%8"SCNx32":%n",&phase,&local,&words,&n)!=3 ||
        !n || phase>1 || words!=2400 || strlen(value+n)!=19200 ||
        strspn(value+n,"0123456789abcdefABCDEF")!=19200) {
        error_setg(errp,"PCM16 requires phase:local:960:exact9600bytehex");return;
    }
    uint64_t target;int ch=serial5_pcm16_request(s,&target);
    if (ch<0 || phase!=s->serial5_pcm_phase[0] || local!=s->peripheral24[ch][6]) {
        error_setg(errp,"PCM16 input does not match original enabled nonoverlapping9600B RX bank");return;
    }
    uint8_t data[9600];
    for (unsigned i=0;i<sizeof(data);i++) {
        data[i]=(g_ascii_xdigit_value(value[n+2*i])<<4)|g_ascii_xdigit_value(value[n+2*i+1]);
    }
    if (address_space_write(&address_space_memory,target,MEMTXATTRS_UNSPECIFIED,
                            data,sizeof(data))!=MEMTX_OK) {
        error_setg(errp,"PCM16 DMA write failed; no completion issued");return;
    }
    s->serial5_pcm_blocks[0]++;s->serial5_pcm_words[0]+=2400;
    s->serial5_pcm_irqs[0]+=!!(s->peripheral24[ch][1]&0x80000);
    s->serial5_pcm_phase[0]^=1;
    pdma_progress(s,ch,0);
}

static NikonZ8State fresh(void){NikonZ8State s={0};s.bootstrap_fixtures=s.bus_window_fixture=s.serial_fixture=s.serial5_audio_config_model=1;s.serial5_config[0][0]=0x1000;s.peripheral_global[1]=0x80000000;
 for(unsigned i=0;i<2;i++){uint32_t*r=s.peripheral24[10+i];r[0]=0x80000000u|((25+i)<<24)|2399;r[1]=0x2ae80000;r[4]=0xe200100;r[6]=0x1000+i*9600;}return s;}
int main(void){char input[19300],*hex;unsigned tests=0;NikonZ8State s;Error*e=0;uint64_t target;
 snprintf(input,sizeof(input),"0:1000:960:");hex=input+strlen(input);for(unsigned i=0;i<9600;i++){unsigned byte=(i*7+3)&255;snprintf(hex+2*i,3,"%02x",byte);}
 for(unsigned k=0;k<15;k++){s=fresh();switch(k){case 0:break;
 case 1:s.serial5_config[0][0]|=1;break;case 2:s.serial5_config[0][0]&=~0x1000;break;
 case 3:s.peripheral24[10][4]=0xe200138;break;case 4:s.peripheral24[10][0]--;break;
 case 5:s.peripheral24[10][1]^=1<<26;break;case 6:s.peripheral24[10][2]=8;break;
 case 7:s.peripheral24[10][3]=1;break;case 8:s.peripheral24[10][6]++;break;
 case 9:s.peripheral24[11][6]=0x1000;break;case 10:s.peripheral_global[1]=0;break;
 case 11:s.peripheral24[10][1]|=0x10000;break;case 12:s.serial_fixture=0;break;
 case 13:s.peripheral24[10][0]^=1u<<24;break;case 14:memcpy(s.peripheral24[12],s.peripheral24[10],32);break;}
 assert((serial5_pcm16_request(&s,&target)>=0)==(k==0));tests++;}
 s=fresh();memset(ram,0xa5,sizeof(ram));set_serial5_pcm16_input(&s,input,&e);assert(!e&&completions==1&&s.serial5_pcm_blocks[0]==1&&s.serial5_pcm_irqs[0]==1&&s.serial5_pcm_phase[0]==1);for(unsigned i=0;i<9600;i++)assert(ram[64+i]==((i*7+3)&255));for(unsigned i=0;i<64;i++)assert(ram[i]==0xa5&&ram[64+19200+i]==0xa5);tests++;
 e=0;set_serial5_pcm16_input(&s,input,&e);assert(e&&completions==1);tests++;
 s=fresh();s.serial5_config[0][0]=0;e=0;set_serial5_pcm16_input(&s,input,&e);assert(e&&completions==1);tests++;
 s=fresh();dma_fail=1;e=0;set_serial5_pcm16_input(&s,input,&e);assert(e&&completions==1&&s.serial5_pcm_phase[0]==0);tests++;
 dma_fail=0;s=fresh();hex[0]='z';e=0;set_serial5_pcm16_input(&s,input,&e);assert(e&&completions==1);tests++;
 hex[0]='0';hex[19199]=0;e=0;set_serial5_pcm16_input(&s,input,&e);assert(e&&completions==1);tests++;
 s=fresh();s.serial5_pcm_phase[0]=1;snprintf(input,sizeof(input),"1:3580:960:");hex=input+strlen(input);for(unsigned i=0;i<9600;i++)snprintf(hex+2*i,3,"%02x",(i*11+7)&255);e=0;set_serial5_pcm16_input(&s,input,&e);assert(!e&&completions==2&&s.serial5_pcm_phase[0]==0);for(unsigned i=0;i<9600;i++)assert(ram[64+9600+i]==((i*11+7)&255));tests++;
 printf("PASS %u gate/bytes/negative cases; 9600 exact bytes =4800 int16, no completion after stop/bad tuple\n",tests);return 0;}
