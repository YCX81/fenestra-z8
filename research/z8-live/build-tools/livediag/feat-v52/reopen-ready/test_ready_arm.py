#!/usr/bin/env python3
"""Actual compiled guard/open code; time/native APIs are explicit test boundaries."""
from pathlib import Path
import struct, json, hashlib, sys
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn import arm64_const as A
D=Path(__file__).resolve().parent
CODE=0x40100000; STOP=0x40200000; W=0x89f5ff000; STACK=0xa13000000
INST=0xa02d41848; ARENA=0x8a2e9b000
STATE=0xa02d3274c; RESULT=0xa02d32788; NATIVE=0xa02d32758
VDI=0xa02d3d980; VDI_ARENA=0xa02d3d950; CLIENT=0xa02e98480; SLOT=0xa02d30118
cases=[]
FINAL = Path(sys.argv[1]) if len(sys.argv) > 1 else None
def case(name, state=1, result=0, changes=(), ticks=(), expect=0, delays=None):
    cases.append(dict(name=name,state=state,result=result,changes=list(changes),ticks=list(ticks),expect=expect,delays=delays))
case('already ready',2,delays=0)
case('early ACK state1 then ready',ticks=[(3,'I',STATE,2)],delays=3)
case('request queued state0 then state1 then ready',state=0,changes=[('Q',NATIVE,0)],ticks=[(2,'Q',NATIVE,INST),(2,'I',STATE,1),(5,'I',STATE,2)],delays=5)
case('stale worker result not read until completed',result=0xffffffff,ticks=[(3,'I',RESULT,0),(3,'I',STATE,2)],delays=3)
case('completed worker error',2,result=0xffffffff,expect=-12,delays=0)
case('pending init timeout',expect=-13,delays=1000)
case('queued init timeout',0,changes=[('Q',NATIVE,0)],expect=-13,delays=1000)
case('missing instance',changes=[('Q',NATIVE,0)],expect=-12,delays=0)
case('inactive instance',changes=[('I',INST,0)],expect=-12,delays=0)
case('instance substituted during init',ticks=[(2,'Q',NATIVE,INST+0x100)],expect=-12,delays=2)
case('state regresses after binding',ticks=[(2,'I',STATE,0)],expect=-12,delays=2)
for state in (3,4,5,0xffffffff):case(f'wrong worker state {state}',state,expect=-12,delays=0)
for name,typ,addr,value in [
 ('invalid stream','I',W+4,2),('VDI stopped','Q',VDI,0),('VDI replaced','Q',VDI,0x17000000),
 ('arena replaced','Q',VDI_ARENA,ARENA+4096),('own client removed','B',CLIENT+4,255),
 ('own client rebound','B',CLIENT+4,1),('own slot closed','I',SLOT,0),
 ('other slot used','I',SLOT+12,1),('release requested','I',W+36,1),
 ('close previously failed','I',W+1053564,1)]:
    case(name,changes=[(typ,addr,value)],expect=-12,delays=0)
    if name!='invalid stream':case(name+' while waiting',ticks=[(2,typ,addr,value)],expect=-12,delays=2)
for c in range(4):case(f'other client {c} active',changes=[('B',CLIENT+c,0)],expect=-12,delays=0)
results=[]
def execute(codec,c,kind='ready',set_rc=0,pre_state=0):
    if FINAL:
        from elftools.elf.elffile import ELFFile
        with FINAL.open('rb') as f:
            elf=ELFFile(f)
            syms={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
            chunks=[(s['sh_addr'],s.data()) for s in elf.iter_sections() if s['sh_flags']&2 and s['sh_size']]
        syms['test_ready']=syms['w5_params_ready'];syms['test_open']=syms['open_stream']
    else:
        syms={s[2]:int(s[0],16)for l in (D/f'{kind}-codec{codec}.nm').read_text().splitlines() if len(s:=l.split())==3}
        chunks=[(CODE,(D/f'{kind}-codec{codec}.bin').read_bytes())]
    u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
    for addr,n in [(CODE,0x20000),(0x401e0000,0x20000),(STOP,0x2000),(W,0x103000),(STACK,0x10000),
        (0xa02d30000,0x20000),(0xa02e98000,4096),(0xa03bd0000,4096),
        (0x40c33000,0x2000),(0x402f2000,0x6000),(0x40187000,4096),(0x401c6000,4096)]:u.mem_map(addr,n)
    for address,data in chunks:u.mem_write(address,data)
    def p(k,a,v):u.mem_write(a,struct.pack('<'+k,v))
    def q(k,a):return struct.unpack('<'+k,u.mem_read(a,struct.calcsize(k)))[0]
    for k,a,v in [('I',W+8,1024),('I',W+12,682),('Q',W+48,ARENA),
        ('Q',VDI,0x16010000),('Q',VDI_ARENA,ARENA),('I',SLOT,1),('I',STATE,c['state']),
        ('Q',NATIVE,INST),('I',INST,1),('I',INST+12,1 if codec else 3),('I',RESULT,c['result']),
        ('Q',0xa02d3d9d8,ARENA+0x200000)]:p(k,a,v)
    u.mem_write(CLIENT,b'\xff\xff\xff\xff\0')
    for v in c['changes']:p(*v)
    if kind=='open':p('Q',VDI,0)
    clock=0;delays=0;calls=[];native_writes=[]
    system={}
    md=Cs(CS_ARCH_ARM64,CS_MODE_ARM)
    md.skipdata=True
    for address,data in chunks:
        for x in md.disasm(data,address):
            if x.mnemonic=='mrs' and ('cntpct_el0' in x.op_str or 'cntfrq_el0' in x.op_str):
                reg,what=x.op_str.split(', ');system[x.address]=(getattr(A,'UC_ARM64_REG_'+reg.upper()),what)
    def hook(uc,pc,size,user):
        nonlocal clock,delays
        if pc in system:
            r,what=system[pc];uc.reg_write(r,1000000 if what=='cntfrq_el0' else clock);uc.reg_write(A.UC_ARM64_REG_PC,pc+4);return
        rc=0
        if pc==0x40187b00:
            assert uc.reg_read(A.UC_ARM64_REG_W0)==1
            delays+=1;clock+=1000
            for at,k,a,v in c['ticks']:
                if at==delays:p(k,a,v)
        elif pc==0x402f2d58:
            calls.append('BRINGUP');p('Q',VDI,0x16010000);p('I',STATE,pre_state);rc=1
        elif pc in (0x40c33834,0x40c333d0,0x40c33480,0x402f6c10):
            calls.append({0x40c33834:'OPEN',0x40c333d0:'CONFIGURE',0x40c33480:'REGISTER',0x402f6c10:'PARAMS'}[pc])
        elif pc==0x40c338ac:
            calls.append('SET_PARAMS');p('I',STATE,c['state']);rc=set_rc
        elif pc==0x40c33d98:
            calls.append('HEADERS');assert q('I',STATE)==2 and q('I',RESULT)==0
            assert q('Q',W+1057016)==q('Q',NATIVE)==INST
            rc=1 # stop after proving HEADERS is reachable only when ready
        elif pc in (0x401c6cb4,0x401c6d00,0x401c6cd0):
            dst=uc.reg_read(A.UC_ARM64_REG_X0);src=uc.reg_read(A.UC_ARM64_REG_X1);n=uc.reg_read(A.UC_ARM64_REG_X2)
            u.mem_write(dst,bytes(u.mem_read(src,n)) if pc==0x401c6cb4 else bytes([src&255])*n)
        else:return
        uc.reg_write(A.UC_ARM64_REG_X0,rc&0xffffffff);uc.reg_write(A.UC_ARM64_REG_PC,uc.reg_read(A.UC_ARM64_REG_X30))
    def onwrite(uc,access,a,n,value,user):
        if 0xa02d30000<=a<0xa02d50000 or CLIENT<=a<CLIENT+5:native_writes.append((a,n,value))
    u.hook_add(UC_HOOK_CODE,hook);u.hook_add(UC_HOOK_MEM_WRITE,onwrite)
    u.reg_write(A.UC_ARM64_REG_X0,W);u.reg_write(A.UC_ARM64_REG_X30,STOP);u.reg_write(A.UC_ARM64_REG_SP,STACK+0xfff0)
    u.emu_start(syms['test_'+kind],STOP,count=300000)
    assert u.reg_read(A.UC_ARM64_REG_PC)==STOP,(codec,c['name'],'no bounded return')
    rc=u.reg_read(A.UC_ARM64_REG_W0);rc=rc-(1<<32) if rc>>31 else rc
    assert rc==c['expect'],(codec,c['name'],rc,c['expect'])
    assert c['delays'] is None or delays==c['delays'],(codec,c['name'],delays)
    assert not native_writes,(c['name'],native_writes)
    return dict(codec=codec,case=c['name'],kind=kind,result=rc,delays=delays,calls=calls,
        native_memory_writes=0,binding=q('Q',W+1057016),passed=True)
for codec in ((1,) if FINAL else (0,1)):
    for c in cases:results.append(execute(codec,c))
    bad=dict(name='wrong codec discriminator',state=1,result=0,changes=[('I',INST+12,3 if codec else 1)],ticks=[],expect=-12,delays=0)
    results.append(execute(codec,bad))
    for name,state,rc,ticks,expected,pre in [
        ('full open waits for ready',1,0,[(3,'I',STATE,2)],-9,0),
        ('full open immediate ready',2,0,[],-9,0),
        ('SET_PARAMS timeout does not become success',2,5,[],-8,0),
        ('SET_PARAMS generic error does not proceed',1,1,[],-8,0),
        ('preexisting worker2 prevents request',2,0,[],-12,2),
        ('full open readiness timeout',1,0,[],-13,0)]:
        c=dict(name=name,state=state,result=0,changes=[],ticks=ticks,expect=expected,delays=None)
        r=execute(codec,c,'open',rc,pre)
        assert r['calls'].count('SET_PARAMS')==(0 if pre else 1)
        assert ('HEADERS' in r['calls'])==(expected==-9)
        results.append(r)
report=dict(passed=True,count=len(results),source_sha256=hashlib.sha256((FINAL.parent.parent/'work/src-fix37/codec/live_w5enc.c' if FINAL else D/'work/src-fix37/codec/live_w5enc.c').read_bytes()).hexdigest(),
    tests=results,limitations='Compiled production C runs as ARM. Native APIs, scheduling and clock are test boundaries, not a full Nikon worker or QEMU integration.')
if FINAL:report.update(elf_path=str(FINAL),elf_sha256=hashlib.sha256(FINAL.read_bytes()).hexdigest())
(D/('final-ready-arm-tests.json' if FINAL else 'ready-arm-tests.json')).write_text(json.dumps(report,indent=2)+'\n')
print(f'PASS {len(results)} actual ARM guard/open cases')
