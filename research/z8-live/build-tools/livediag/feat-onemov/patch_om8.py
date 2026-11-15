"""V61: resolve a RAW photo's Live through Nikon's paired-copy lookup.

Builds on the unforced V60 capture rule. The current photo/DCF group remains
the playback anchor; only open/stat of its virtual MOV go to the verified
Live JPEG's volume. This script only edits the supplied source tree.
"""
from pathlib import Path
import os
import subprocess
import sys

W = Path(sys.argv[1])
HERE = Path(__file__).resolve().parent
assert not os.environ.get('OM_FORCE') and not os.environ.get('OM_NORULE'), 'test-only V60 flags are not a V61 production base'
subprocess.run([sys.executable, str(HERE / 'patch_om7.py'), str(W)], check=True)

def sub(path, old, new):
    p = W / path
    text = p.read_text()
    assert text.count(old) == 1, (path, old[:100], text.count(old))
    p.write_text(text.replace(old, new))

R = 'pb-fix37/runtime.c'
sub('pb-fix37/runtime.h', '    char nb_from[64],nb_to[64];\n',
    '    char nb_from[64],nb_to[64];\n'
    '    /* v61: cached pairing and DCF ownership are scoped to one media epoch. */\n'
    '    uint32_t lookup_epoch,nb_epoch;\n'
    '    uint64_t lookup_us;\n'
    '    _Atomic uint32_t pair_lock;\n'
    '    uint32_t nb_card_epoch;_Atomic uint32_t card_epoch[2];\n')
sub('pb-fix37/runtime.h', '    uint32_t started,error,polls,current_id,cache_valid,blocked_key_epoch;',
    '    uint32_t started,error,polls,current_id;_Atomic uint32_t cache_valid;uint32_t blocked_key_epoch;')
sub(R, '/* ---- one-MOV bridge ---- */',
    '/* Pair publication and UI bridge setup never wait on each other. */\n'
    'static int pair_lock(PbRuntime*r){uint32_t z=0;return atomic_compare_exchange_strong_explicit(&r->pair_lock,&z,1,memory_order_acquire,memory_order_relaxed);}\n'
    '/* ---- one-MOV bridge ---- */')

sub(R, 'return r&&LOAD(&r->nb_stage)>=2u&&path&&!memcmp(path,r->nb_from,30);}',
    'return r&&LOAD(&r->nb_stage)>=2u&&r->nb_epoch==LOAD(&r->media_epoch)&&path&&!memcmp(path,r->nb_from,30);}')
sub(R, ' if(!o||!r||LOAD(&r->nb_stage)<2u)return o;',
    ' if(!o||!r||LOAD(&r->nb_stage)<2u||r->nb_epoch!=LOAD(&r->media_epoch))return o;')
sub(R, 'static uint16_t *nb_mask(uint32_t id,int set){',
    'static uint16_t *nb_mask(PbRuntime*r,uint32_t id,int set){')
sub(R, 'node>=slot&&node<slot+0x222188u?', 'node>=slot&&node<=slot+0x222188u-0x26u?')
sub(R, ' if(m){if(set)*m|=0x20u;else *m&=(uint16_t)~0x20u;}',
    ' /* Never borrow a real MOV member; clear only the node we actually changed. */\n'
    ' if(m){if(set){if(*m&0x20u)m=0;else *m|=0x20u;}\n'
    '  else if((uintptr_t)m==r->nb_node&&r->nb_card_epoch==LOAD(&r->card_epoch[card]))*m&=(uint16_t)~0x20u;}')
sub(R, ' if(LOAD(&r->nb_stage)>=2u){if(r->nb_bit)nb_mask(r->nb_photo,0);r->nb_bit=0;STORE(&r->nb_stage,0);}',
    ' if(LOAD(&r->nb_stage)>=2u){if(r->nb_bit)nb_mask(r,r->nb_photo,0);r->nb_bit=0;STORE(&r->nb_stage,0);}')
sub(R, '  FN(0x403ec3a0,void(*)(uint32_t,uint32_t))(0x4910,r->nb_photo);\n',
    '  uint32_t selected=FN(0x403ec398,uint32_t(*)(uint32_t))(0x4910);\n'
    '  if(r->nb_card_epoch==LOAD(&r->card_epoch[r->nb_photo>>29&1u])&&(selected&~15u)==(r->nb_photo&~15u))\n'
    '   FN(0x403ec3a0,void(*)(uint32_t,uint32_t))(0x4910,r->nb_photo);\n')
sub(R, ' if(LOAD(&r->nb_stage)==2u)return;\n',
    ' if(LOAD(&r->nb_stage)||!pair_lock(r))return;\n')
sub(R, ' if(!playback_owner()||!current(&id)||id!=r->current_id||!r->cache_valid||!r->nb_w||\n    live_sync_file(to,r->native_path,LIVE_FILE_MOV))',
    ' if(!playback_owner()||!current(&id)||(id>>29&3u)>1u||id!=r->current_id||id!=r->cached_id||!r->cache_valid||!r->nb_w||LOAD(&r->shutdown)||\n'
    '    r->lookup_epoch!=LOAD(&r->media_epoch)||live_sync_file(to,r->manifest.native_path,LIVE_FILE_MOV))')
sub(R, '{r->nb_fail=1;r->nb_cur=id;pb_nb_observe(&r->nb_request,1);return;}',
    '{STORE(&r->pair_lock,0);r->nb_fail=1;r->nb_cur=id;pb_nb_observe(&r->nb_request,1);return;}')
sub(R, ' if(LOAD(&r->nb_stage)==3u&&r->nb_bit&&r->nb_photo!=id)nb_mask(r->nb_photo,0);\n uint16_t*m=nb_mask(id,1);r->nb_node=(uintptr_t)m;r->nb_bit=m!=0;',
    ' r->nb_epoch=r->lookup_epoch;r->nb_card_epoch=LOAD(&r->card_epoch[id>>29&1u]);\n'
    ' uint16_t*m=r->nb_epoch==LOAD(&r->media_epoch)?nb_mask(r,id,1):0;r->nb_node=(uintptr_t)m;r->nb_bit=m!=0;')
sub(R, ' STORE(&r->nb_stage,m?2u:0u);',
    ' STORE(&r->nb_stage,m?2u:0u);STORE(&r->pair_lock,0);')

sub(R, ' if(live_sync_file(p,r->native_path,LIVE_FILE_MOV))return;',
    ' if(live_sync_file(p,r->manifest.native_path,LIVE_FILE_MOV))return;')
p = W / R
t = p.read_text()
a = t.index('static void lookup(PbRuntime*r,uint32_t id){')
b = t.index('/* ---- preview sound', a)
t = t[:a] + (HERE / 'v61/lookup.cfrag').read_text() + t[b:]
p.write_text(t)
sub(R, '  else {\n   if(id!=lastid){hide(r);r->current_id=id;lastid=id;lookup(r,id);r->blocked_key_epoch=0;}',
    '  else if(!LOAD(&r->nb_stage)){\n'
    '   if((id!=lastid||r->lookup_epoch!=LOAD(&r->media_epoch)||(!r->cache_valid&&now_us()-r->lookup_us>=1000000u))&&pair_lock(r)){\n'
    '    if(!LOAD(&r->nb_stage)){hide(r);r->cache_valid=0;r->current_id=id;lastid=id;lookup(r,id);r->blocked_key_epoch=0;}\n'
    '    STORE(&r->pair_lock,0);}\n')
sub(R, 'r->cache_valid&&LOAD(&r->nb_stage)!=2u&&',
    'r->cache_valid&&!LOAD(&r->nb_stage)&&!LOAD(&r->shutdown)&&r->lookup_epoch==LOAD(&r->media_epoch)&&')
sub(R, '(r->playing||!r->blocked_key_epoch)&&current(&id)&&id==r->current_id;',
    '(r->playing||!r->blocked_key_epoch)&&current(&id)&&id==r->current_id&&id==r->cached_id;')
sub(R, 'if(r->cache_valid&&!r->blocked_key_epoch&&!r->display.display.attached&&ready_to_write(r,key)){',
    'if(r->cache_valid&&r->lookup_epoch==LOAD(&r->media_epoch)&&!LOAD(&r->nb_stage)&&!r->blocked_key_epoch&&!r->display.display.attached&&ready_to_write(r,key)){')
sub(R, 'int gone=!playback_owner()||LOAD(&r->shutdown);',
    'int gone=!playback_owner()||LOAD(&r->shutdown)||r->nb_epoch!=LOAD(&r->media_epoch);')
sub(R, 'r->nb_cleanups++;if(!gone){', 'r->nb_cleanups++;if(playback_owner()){')
sub(R, ' if(event==7||event==24){',
    ' /* Native card lifecycle events (including format) carry slot at +40. */\n'
    ' if(event==7||(event>=22u&&event<=31u)){')
sub(R, '  else {atomic_fetch_add_explicit(&r->media_epoch,1,memory_order_acq_rel);',
    '  else {uint32_t card=*(const uint32_t*)((const uint8_t*)message+40);\n'
    '   /* Invalidate old pairings before publishing a new source-card identity. */\n'
    '   atomic_fetch_add_explicit(&r->media_epoch,1,memory_order_acq_rel);\n'
    '   for(uint32_t i=0;i<2u;i++)if(card==i||card>1u)atomic_fetch_add_explicit(&r->card_epoch[i],1,memory_order_acq_rel);')
sub(R, ' PbRuntime*r=pb_runtime_get();LiveManifest m={0};int valid=r&&r->cache_valid&&r->cached_id==id;',
    ' PbRuntime*r=pb_runtime_get();LiveManifest m={0};int valid=r&&r->cache_valid&&r->cached_id==id&&\n'
    '  r->lookup_epoch==LOAD(&r->media_epoch)&&!memcmp(r->native_path,r->manifest.native_path,30);')
sub(R, ' LiveManifest m={0};int valid=r&&r->cache_valid&&r->cached_id==id;if(valid)m=r->manifest;',
    ' LiveManifest m={0};int valid=r&&r->cache_valid&&r->cached_id==id&&\n'
    '  r->lookup_epoch==LOAD(&r->media_epoch)&&!memcmp(r->native_path,r->manifest.native_path,30);if(valid)m=r->manifest;')

# The I420 preview player was removed in V54. Its exported renderer has no
# remaining caller; reclaim its code within the existing PB bank for pairing.
p = W / 'pb-fix37/ui.c'
t = p.read_text()
a = t.index('static uint8_t clamp(')
b = t.index('#include "runtime.h"', a)
t = t[:a] + '/* v61: the native movie page owns video rendering. */\n' + t[b:]
p.write_text(t)
print('om8 patched: RAW anchor, native paired-copy lookup, epoch-scoped bridge')
