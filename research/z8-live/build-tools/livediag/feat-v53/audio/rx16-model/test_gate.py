from pathlib import Path
import subprocess,json
p=Path(__file__).resolve().parent
inc=(p/'pcm16.inc').read_text().split('static char *get_serial5_pcm16_state')[0]
prefix=r'''
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
'''
main=r'''
static NikonZ8State fresh(void){NikonZ8State s={0};s.bootstrap_fixtures=s.bus_window_fixture=s.serial_fixture=s.serial5_audio_config_model=1;s.serial5_config[0][0]=0x1000;s.peripheral_global[1]=0x80000000;
 for(unsigned i=0;i<2;i++){uint32_t*r=s.peripheral24[10+i];r[0]=0x80000000u|((25+i)<<24)|2399;r[1]=0x2ae80000;r[4]=0xe200100;r[6]=0x1000+i*9600;}return s;}
int main(void){char input[19300],*hex;unsigned tests=0;NikonZ8State s;Error*e=0;uint64_t target;
 snprintf(input,sizeof(input),"0:1000:960:");hex=input+strlen(input);for(unsigned i=0;i<9600;i++){unsigned byte=(i*7+3)&255;snprintf(hex+2*i,3,"%02x",byte);}
 for(unsigned k=0;k<15;k++){s=fresh();switch(k){case0:break;
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
'''.replace('case0:','case 0:')
(p/'gate-test.c').write_text(prefix+inc+main)
subprocess.run(['clang','-std=c11','-O1','-g','-fsanitize=address,undefined',str(p/'gate-test.c'),'-o',str(p/'gate-test')],check=True)
r=subprocess.run([str(p/'gate-test')],check=True,capture_output=True,text=True);print(r.stdout)
(p/'gate-tests.json').write_text(json.dumps({'passed':True,'output':r.stdout,'scope':'Actual proposed device request/parser body; RAM translation and device completion boundary mocked, no original OS/IRQ claimed.'},indent=2)+'\n')
