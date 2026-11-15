#!/usr/bin/env python3
"""Actual compiled open_stream caller + original native asynchronous reply.

Bringup/configuration and RTOS wait are boundaries. The original HEADERS service
stores pointers and times out, then its original completion writes a late reply.
"""
from pathlib import Path
import hashlib, json, struct, subprocess
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import *

HERE=Path(__file__).resolve().parent
V52=HERE.parents[1]
FW=V52.parent/'feat-v50/integration-writer/v51d/q/main.main'
D=FW.read_bytes(); BASE=0x40160000
CODE, STUB, END=0x47000000,0x470f0000,0x470ff000
STACK, POOL, TEMP=0x4e010000,0x89f5ff000,0x4e030000

def run_case(name, source):
    c=HERE/f'{name}-probe.c'; obj=HERE/f'{name}-probe.o'
    c.write_text('#include "'+str(source)+'"\nconst unsigned long test_layout[]={sizeof(struct live_w5),offsetof(struct live_w5,scratch),offsetof(struct live_w5,close_failed)};\n')
    subprocess.run(['clang','--target=aarch64-none-elf','-ffreestanding','-O2','-ffunction-sections','-fdata-sections','-I',str(source.parent/'freestanding'),'-c',str(c),'-o',str(obj)],check=True)
    with obj.open('rb') as f:
        elf=ELFFile(f); syms=elf.get_section_by_name('.symtab')
        fun=next(s for s in syms.iter_symbols() if s.name=='open_stream')
        lay=next(s for s in syms.iter_symbols() if s.name=='test_layout')
        layout=struct.unpack('<3Q',elf.get_section(lay['st_shndx']).data()[lay['st_value']:lay['st_value']+24])
        text=bytearray(elf.get_section(fun['st_shndx']).data())
        consts={}; stubnames={}; stubs={}
        def target(sym,add):
            sh=sym['st_shndx']
            if sh=='SHN_UNDEF' or (isinstance(sh,int) and elf.get_section(sh).name.startswith('.text')):
                n=sym.name or elf.get_section(sh).name
                if n not in stubs:
                    stubs[n]=STUB+len(stubs)*16;stubnames[stubs[n]]=n
                return stubs[n]+add
            if sh not in consts:consts[sh]=(CODE+0x10000+sh*0x1000,elf.get_section(sh).data())
            return consts[sh][0]+sym['st_value']+add
        for r in elf.get_section_by_name('.rela.text.open_stream').iter_relocations():
            off=r['r_offset']; pc=CODE+off; t=target(syms.get_symbol(r['r_info_sym']),r['r_addend'])
            ins=struct.unpack_from('<I',text,off)[0];typ=r['r_info_type']
            if typ==283: ins=(ins&0xfc000000)|(((t-pc)>>2)&0x3ffffff)
            elif typ==275:
                q=((t&~4095)-(pc&~4095))>>12
                ins=(ins&~((3<<29)|(0x7ffff<<5)))|((q&3)<<29)|(((q>>2)&0x7ffff)<<5)
            elif typ in (277,299):
                q=(t&4095)>>(4 if typ==299 else 0)
                ins=(ins&~(4095<<10))|(q<<10)
            else:raise AssertionError(typ)
            struct.pack_into('<I',text,off,ins)
    u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
    for a,n in [(BASE,(len(D)+4095)&~4095),(CODE,0x100000),(0x4e000000,0x50000),(POOL,0x110000),
                (0xa02d30000,0x20000),(0xa02e98000,0x1000),(0xa03bd0000,0x1000)]:u.mem_map(a,n)
    u.mem_write(BASE,D);u.mem_write(CODE,bytes(text))
    for a,b in consts.values():u.mem_write(a,b)
    for a in stubnames:u.mem_write(a,bytes.fromhex('c0035fd6'))
    def w32(a,v):u.mem_write(a,struct.pack('<I',v&0xffffffff))
    def w64(a,v):u.mem_write(a,struct.pack('<Q',v))
    def r64(a):return struct.unpack('<Q',u.mem_read(a,8))[0]
    header_calls=[]
    def hook(uc,pc,size,data):
        args=[uc.reg_read(r) for r in (UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3)]
        n=stubnames.get(pc)
        if n=='memset':uc.mem_write(args[0],bytes([args[1]&255])*args[2])
        elif n=='memcpy':uc.mem_write(args[0],bytes(uc.mem_read(args[1],args[2])))
        elif n and n.startswith('.text.table'):pass
        elif n:raise AssertionError(n)
        elif pc==0x402f2d58:
            w64(0xa02d3d980,0x16010000);uc.mem_write(0xa02e98484,b'\0');uc.reg_write(UC_ARM64_REG_W0,1)
        elif pc in (0x40c33834,0x40c333d0,0x40c33480,0x402f6c10,0x40c338ac):uc.reg_write(UC_ARM64_REG_W0,0)
        elif pc==0x40c33d98:
            header_calls.append(args);uc.reg_write(UC_ARM64_REG_PC,0x40c38af8);return
        elif pc==0x40d2bc8c:pass
        elif pc==0x40d2bdac:uc.reg_write(UC_ARM64_REG_W0,0) # Real service timeout branch.
        else:return
        uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR))
    u.hook_add(UC_HOOK_CODE,hook)
    w32(POOL+8,1024);w32(POOL+12,682);w64(POOL+48,0x8a2c9b000)
    u.reg_write(UC_ARM64_REG_SP,STACK);u.reg_write(UC_ARM64_REG_LR,END);u.reg_write(UC_ARM64_REG_X0,POOL)
    u.emu_start(CODE,END,count=100000)
    assert u.reg_read(UC_ARM64_REG_PC)==END
    assert u.reg_read(UC_ARM64_REG_W0)==0xfffffff7 # open_stream returns -9.
    assert len(header_calls)==1
    ptrs=[r64(a) for a in (0xa02d30f28,0xa02d30f38,0xa02d30f48)]
    assert ptrs==header_calls[0][1:]
    # Reuse the caller stack; delayed completion must no longer write here.
    u.mem_write(STACK-4096,bytes([0x5a])*4096)
    scratch=POOL+layout[1]
    # Adjacent RUN/SUBMIT scratch users must not overlap the persistent reply.
    u.mem_write(scratch+0x700,bytes([0x71])*16)
    u.mem_write(scratch+0x800,bytes([0x82])*64)
    callback_sp=0x4e020000; lens=[2,25,9,0]; hdr=0x8a1234000; count=52
    u.mem_write(TEMP,struct.pack('<4I',*lens))
    w64(callback_sp+0xa58,hdr);w32(callback_sp+0xa60,count);w64(callback_sp+0xa18,TEMP+0x100)
    u.reg_write(UC_ARM64_REG_SP,callback_sp);u.reg_write(UC_ARM64_REG_X22,TEMP);u.reg_write(UC_ARM64_REG_X21,0)
    u.emu_start(0x40c3ccfc,0x40c3cd88,count=1000)
    assert r64(ptrs[0])==hdr
    assert bytes(u.mem_read(ptrs[1],4))==struct.pack('<I',count)
    assert bytes(u.mem_read(ptrs[2],16))==struct.pack('<4I',*lens)
    stack=bytes(u.mem_read(STACK-4096,4096));changed=sum(x!=0x5a for x in stack)
    if name=='candidate':
        assert ptrs==[scratch+0x720,scratch+0x728,scratch+0x72c]
        assert changed==0
        assert ptrs[0]%8==0 and ptrs[2]+16<=scratch+0x740
        assert bytes(u.mem_read(scratch+0x700,16))==bytes([0x71])*16
        assert bytes(u.mem_read(scratch+0x800,64))==bytes([0x82])*64
    else:assert changed>0
    return dict(source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),layout=layout,
                header_pointers=[hex(p) for p in ptrs],caller_stack_bytes_changed=changed,
                native_late_reply_written=True,open_stream_bytes=len(text))

results={n:run_case(n,p) for n,p in [
    ('baseline',V52/'integration-e/work/src-fix37/codec/live_w5enc.c'),
    ('candidate',HERE/'work/src-fix37/codec/live_w5enc.c')]}
report=dict(passed=True,results=results,
    boundaries=['Native bringup/config/register/SET_PARAMS success are stubs.',
                'HEADERS original service uses a timeout event-wait stub.',
                'The original native completion block writes after the compiled caller returns.'],
    limitation='This establishes software pointer lifetime under a late completion, not that the Eclose run timed out or acoustic/hardware behavior.')
(HERE/'test-result.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
