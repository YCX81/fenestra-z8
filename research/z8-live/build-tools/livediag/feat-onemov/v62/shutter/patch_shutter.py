#!/usr/bin/env python3
"""Apply the V62 finite-profile shutter candidate to a copied V61 source tree.

Does not build, connect to hardware, or copy a firmware to media.
"""
from pathlib import Path
import hashlib
import json
import sys

work = Path(sys.argv[1]).resolve()
baseline = Path(__file__).resolve().parents[2] / 'v61' / 'work'
if work == baseline.resolve():
    raise ValueError('Use an isolated copy; the delivered V61 source is immutable')

def sha(b):
    return hashlib.sha256(b).hexdigest()

def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError('Unexpected source text: ' + old[:90])
    return text.replace(old, new)

source = work / 'src-fix37/audio/live_shutter_z8.c'
hooks = work / 'src-fix37/hooks.S'
before = {str(p.relative_to(work)): sha(p.read_bytes()) for p in [source, hooks]}
expected = {
    'src-fix37/audio/live_shutter_z8.c': '2c9b308ff90d6f234ffa4c87cb170444b05cd80c4908f8f0afa9190f760d2190',
    'src-fix37/hooks.S': '3b9e0c9825bc9bb7c226eeec244cdd5a5d1fc1a3d439d44827c15bdb5c14b53d',
}
if before != expected:
    raise ValueError('Input does not match the verified V61 shutter/hook sources: ' + str(before))

t = source.read_text()
t = replace_once(t,
    'static SH int allowed(struct live_runtime*r){(void)r;return 0;} /* v56: hand-off disabled */',
    '''static SH int mic_wanted(struct live_runtime*r){
 return r&&!LOAD(&r->shutdown)&&LOAD(&r->requested)&&!LOAD(&r->crumb_pad)&&
  word(UINT64_C(0xa03bd062c))==0&&
  r->audio.active&&!LOAD(&r->audio.cancelled)&&!r->audio.frozen;
}
static SH int allowed(struct live_runtime*r){
 return mic_wanted(r)&&!live_shutter_mute_get()&&
  !*(volatile uint8_t *)(uintptr_t)UINT64_C(0xa03bc7008);
}
/* Prepare is the common gate for all native shutter profiles. The stock
 * producer copies exactly 24 bytes synchronously; byte8=0 makes SoundTask
 * inhibit both prepare and fire. Keep the caller's argument and stock mute
 * unchanged. This small adapter uses primary text, not the bounded SFX bank. */
uint32_t live_shutter_prepare(void *argument){
 struct live_runtime*r=runtime();
 if(r&&!LOAD(&r->shutdown)&&LOAD(&r->requested)&&
    word(UINT64_C(0xa03bd062c))==0&&live_shutter_mute_get()){
  uintptr_t copy[3];const uintptr_t *p=argument;
  copy[0]=p[0];copy[1]=p[1];copy[2]=p[2];
  ((uint8_t*)copy)[8]=0;
  return FN(0x40f9cccc,uint32_t(*)(void*))(copy);
 }
 return FN(0x40f9cccc,uint32_t(*)(void*))(argument);
}''')
t = replace_once(t,
    '''static SH int normal(struct laz8_msg*m){
 uintptr_t p=params(m);
 return m->id==START&&m->param==0x40e32ed8u&&m->tail==(uintptr_t)m&&p&&
  ptr((uintptr_t)m->payload+8)==0x40e3159cu&&
  (word(p+12)==1||word(p+12)==2)&&word(p+8)==1&&ptr(p+24)==NATIVE_DONE;
}''',
    '''/* The expanded selector also uses primary text; SFX bank is bounded. */
static __attribute__((noinline)) int normal(struct laz8_msg*m){
 /* Do not interpret another message kind's payload as a shutter pointer. */
 if(m->id!=START||m->param!=0x40e32ed8u||m->tail!=(uintptr_t)m||!m->payload||
    ptr((uintptr_t)m->payload+8)!=0x40e3159cu)return 0;
 uintptr_t p=params(m);if(!p)return 0;
 uint32_t tone=word(p+12);
 /* Native finite photo tones: 1/2; 10/11/12,14/15/16,18/19/20,22/23/24.
  * The 6/13/17/21/25 loop branch and every non-native envelope stay native. */
 return tone<25u&&((UINT32_C(0x01dddc06)>>tone)&1u)&&
  word(p+8)==1&&ptr(p+24)==NATIVE_DONE;
}''')
h = replace_once(hooks.read_text(),
    ' b live_stock_shutter_prepare\n',
    ' b live_shutter_prepare\n')
t = replace_once(t,
    ' if(want&&allowed(r)){\n  if(live_audio_z8_resume',
    ' /* Muting shutter sound cancels SFX, but must not disable Live recording. */\n'
    ' if(want&&mic_wanted(r)){\n  if(live_audio_z8_resume')
source.write_text(t)
hooks.write_text(h)
after = {str(p.relative_to(work)): sha(p.read_bytes()) for p in [source, hooks]}
print(json.dumps({'work': str(work), 'before': before, 'after': after}, indent=2))
