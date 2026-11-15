#!/usr/bin/env python3
"""Actual V61 SoundTask selection and prepare byte8 mute, explicit inputs.

Settings, exposure inputs, heap/queue/OS and duration are fixtures.
Native parameter copying, profile selection, constructors, driver5 rejection,
callbacks and release all execute from the pinned delivered V61 MAIN.
"""
from pathlib import Path
import hashlib
import json
import struct
from unicorn import UC_HOOK_CODE
from unicorn.arm64_const import *

P = Path(__file__).resolve().parent
legacy = P.parents[2] / 'feat-v53/portrait/shutter/test_sound_cancel.py'
main = P.parents[1] / 'v61/final/main.main'
source = legacy.read_text().split('rows=[]')[0]
source = source.replace("D = (P.parent/'v52g.main').read_bytes(); B = 0x40160000", "D = Path(" + repr(str(main)) + ").read_bytes(); B = 0x40160000")
source = source.replace('ed1e8496fccde443412e527a252de5029a55c749053b9b046da42bfc454aa613',
                        'db0c847b333f0a6e07222f2207435e9bf340ce1d544704f6d801ec3ecc1088e5')
ns = {'__file__': str(legacy)}
exec(compile(source, str(legacy), 'exec'), ns)
u, D, call, reset, state, r32, r64, w32 = (ns[n] for n in ('u','D','call','reset','state','r32','r64','w32'))
for address in (0xa0292b000,0xa029ca000):
    u.mem_map(address,4096)
    off=address-0xa02808000+0x4b13000
    u.mem_write(address,D[off:off+4096])
ins=struct.unpack_from('<I',D,0x40f9cb64-0x40160000)[0]
imm=ins&0x3ffffff
if imm&0x2000000:imm-=0x4000000
gate=0x40f9cb64+imm*4
flags=[]; hits=[]; gates=[]; profile=0; comparison=2
def extra(uc,pc,size,data):
    if pc==0x41012c84:
        assert uc.reg_read(UC_ARM64_REG_W0)==0x02006101
        ns['ret'](profile)
    elif pc in (0x40d2bcc8,0x40d2b88c):ns['ret'](0)
    elif pc==0x40f94f5c:ns['ret'](2) # no future prepare timer in this fixture
    elif pc==gate:
        stock=u.mem_read(0xa03bc7008,1)[0];gates.append(stock)
        uc.reg_write(UC_ARM64_REG_W8,stock);ns['ret']()
    elif pc==0x40c002a0:flags.append(uc.reg_read(UC_ARM64_REG_W1));ns['ret'](0)
    elif pc in (0x40f9caf4,0x40f9cc04):
        hits.append({'pc':hex(pc),'tone':uc.reg_read(UC_ARM64_REG_W0),'duration':uc.reg_read(UC_ARM64_REG_W1)})
    elif pc in (0x40d2bd04,0x40d2bd64):raise AssertionError('Unexpected blocking wait')
u.hook_add(UC_HOOK_CODE,extra)
rows=[]
for enabled in (0,1):
    for profile in range(5):
        for comparison in (0,2):
            if enabled and profile==0 and comparison==0:continue # separate tone0 semantics
            reset();flags.clear();hits.clear();gates.clear()
            w32(0xa03bc700c,60);w32(0xa0292be18,60);w32(0xa0292be1c,60)
            # Real native rational comparison with the native 34/6 threshold:
            # 30/6 < 34/6 selects the prepare/fire pair; 42/6 selects full tone.
            exposure_numerator=30 if comparison==2 else 42
            arg=0x4e005000;payload=struct.pack('<IIQQ',6,exposure_numerator,enabled,1000000)
            u.mem_write(arg,payload);call(0x40f9cccc,arg)
            assert flags==[2] and bytes(u.mem_read(0xa03bc6ff0,24))==payload
            # The native producer stores values, never retains the caller's stack pointer.
            u.mem_write(arg,b'\xa5'*24)
            call(0x40f9c5cc,2)
            selected=[r32(0xa0292be18),r32(0xa0292be1c)]
            envelopes=[]
            while ns['queued']:
                msg=ns['queued'].pop(0);p=r64(r64(msg+40))
                envelopes.append({'tone':r32(p+12),'repeat':r32(p+8),'callback':hex(r64(p+24))})
                call(0x40e34b2c,msg)
            call(0x40f9cd54,0);assert flags==[2,4]
            call(0x40f9c5cc,4)
            while ns['queued']:
                msg=ns['queued'].pop(0);p=r64(r64(msg+40))
                envelopes.append({'tone':r32(p+12),'repeat':r32(p+8),'callback':hex(r64(p+24))})
                call(0x40e34b2c,msg)
            if not enabled:
                assert not hits and not envelopes
                assert u.mem_read(0xa03bc7016,1)[0]==1
            else:
                expected=([1,2] if profile==0 else [4*profile+7,4*profile+8]) if comparison==2 else [4*profile+6]
                assert [h['tone'] for h in hits]==expected,(profile,comparison,hits,expected)
                assert [x['tone'] for x in envelopes]==expected
                assert all(x['repeat']==1 and x['callback']=='0x40f9d194' for x in envelopes)
                if comparison==0:assert not gates # full-tone prepare bypasses old fire-only mute gate
            s=state();assert s['allocated']==s['freed'] and not any(s[x] for x in ('pending','soundtask','sfx'))
            rows.append({'enabled':enabled,'profile':profile,'exposure_rational':[6,exposure_numerator],
                         'selected':selected,'hits':hits[:],
                         'envelopes':envelopes,'fire_mute_hook_calls':len(gates),'passed':True})
# Preserve one native instance while toggling the common prepare gate.
reset();profile=1;comparison=0;w32(0xa03bc700c,60);w32(0xa0292be18,60);w32(0xa0292be1c,60)
for enabled in (1,0,1,0):
    flags.clear();hits.clear();gates.clear()
    arg=0x4e005000;payload=struct.pack('<IIQQ',6,42,enabled,1000000)
    u.mem_write(arg,payload);call(0x40f9cccc,arg);call(0x40f9c5cc,2)
    submitted=[]
    while ns['queued']:
        msg=ns['queued'].pop(0);p0=r64(r64(msg+40));submitted.append(r32(p0+12));call(0x40e34b2c,msg)
    call(0x40f9cd54,0);call(0x40f9c5cc,4)
    assert not ns['queued'] and submitted==([10] if enabled else [])
    assert u.mem_read(0xa03bc7016,1)[0]==(0 if enabled else 1)
    s=state();assert s['allocated']==s['freed'] and not any(s[x]for x in ('pending','soundtask','sfx'))
    rows.append({'case':'same-instance On/Off/On/Off','enabled':enabled,'submitted':submitted,'passed':True})
report={'passed':True,'cases':rows,'case_count':len(rows),'main_sha256':hashlib.sha256(D).hexdigest(),
        'boundaries':['Exact V61 native producer, SoundTask and constructor/rejection/completion instructions execute.',
                      'Settings profile and exposure rational are explicit fixture inputs; the native comparison executes. No persistent camera settings are modified.',
                      'Heap/queue/OS/duration are stubs. Driver5 rejection is real native code; this is not admitted DAC playback.',
                      'Separate tone0 and repeating/bulb protocols are intentionally outside this candidate.']}
(P/'native-profiles.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS',len(rows),'actual V61 native profile and prepare-byte8 cases')
