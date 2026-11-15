"""Unicorn checks for the v42 read/write hooks: arguments reach the stock body (entry+4 via trampoline) unchanged,
the return value comes back, callee-saved regs and SP are preserved, DC ops cover exactly the buffer lines for RAM
buffers and are skipped otherwise."""
import struct,sys
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import *
import capstone
B=0x40160000;img=open(sys.argv[1],'rb').read()
md=capstone.Cs(capstone.CS_ARCH_ARM64,capstone.CS_MODE_ARM)
READ,WRITE=0x402dd6c8,0x402dd78c;RET=0x7e000ff0;ok=True;n=0
def check(c,m):
    global ok,n;n+=1
    if not c:ok=False;print('FAIL',m)
def run(entry,args,buf_base):
    mu=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
    mu.mem_map(0x40000000,0x10000000);mu.mem_write(B,img[:0x40000000+0x10000000-B] if False else img[:0x4000000])
    mu.mem_write(0x402dd000,img[0x402dd000-B:0x402de000-B])
    mu.mem_map(0x7e000000,0x100000)
    mu.mem_write(RET,struct.pack('<I',0xd503201f))
    try:mu.mem_map(buf_base&~0xfffff,0x200000)
    except Exception:pass
    sp=0x7e0f0000;mu.reg_write(UC_ARM64_REG_SP,sp);mu.reg_write(UC_ARM64_REG_X30,RET)
    for i,a in enumerate(args):mu.reg_write(UC_ARM64_REG_X0+i,a)
    for r in range(19,29):mu.reg_write(UC_ARM64_REG_X0+r,0x1111*r)
    # mrs ctr_el0 -> emulate via hook; record dc ops and the body entry
    st=dict(dc=[],body=None)
    body=entry+4
    def code(mu,addr,size,ud):
        w=struct.unpack('<I',mu.mem_read(addr,4))[0]
        if addr==body:
            st['body']=[mu.reg_read(UC_ARM64_REG_X0+i) for i in range(5)];st['body_sp']=mu.reg_read(UC_ARM64_REG_SP)
            mu.reg_write(UC_ARM64_REG_X0,0x5a5a);mu.reg_write(UC_ARM64_REG_PC,RET+4 if False else mu.reg_read(UC_ARM64_REG_X30));
            # emulate stock epilogue: the relocated first insn already moved SP; undo it
            if entry==READ:mu.reg_write(UC_ARM64_REG_SP,mu.reg_read(UC_ARM64_REG_SP)+0x40)
            else:mu.reg_write(UC_ARM64_REG_SP,mu.reg_read(UC_ARM64_REG_SP)+0x60)
            return
        if (w&0xfff00000)==0xd5300000 or (w&0xfffff000)==0xd53b0000:  # mrs
            i=next(md.disasm(bytes(mu.mem_read(addr,4)),addr))
            if 'ctr_el0' in i.op_str:
                rd=w&31;mu.reg_write(UC_ARM64_REG_X0+rd,0x8444c004);mu.reg_write(UC_ARM64_REG_PC,addr+4)
        if (w&0xfff80000)==0xd5080000:  # sys (dc)
            i=next(md.disasm(bytes(mu.mem_read(addr,4)),addr));rt=w&31
            st['dc'].append((i.op_str.split(',')[0],mu.reg_read(UC_ARM64_REG_X0+rt)));mu.reg_write(UC_ARM64_REG_PC,addr+4)
    mu.hook_add(UC_HOOK_CODE,code)
    mu.emu_start(entry,RET,count=200000)
    st['x0']=mu.reg_read(UC_ARM64_REG_X0);st['sp']=mu.reg_read(UC_ARM64_REG_SP)
    st['saved']=all(mu.reg_read(UC_ARM64_REG_X0+r)==0x1111*r for r in range(19,29))
    return st,sp
for entry,name in ((READ,'read'),(WRITE,'write')):
    for buf,size,ram in ((0xa10001010,0x2000,True),(0xe10000000,64,True),(0x40100000,4096,False),(0xa10001000,0,False),(0xfff0000000,64,False)):
        args=(7,buf,size,0x7e080000,0x99)
        st,sp=run(entry,args,buf)
        check(st['body'] is not None,f'{name} reached body')
        check(st['body'] and st['body'][:4]==list(args[:4]) and (entry==READ or st['body'][4]==0x99),f'{name} args {st["body"]}')
        check(st['x0']==0x5a5a,f'{name} return');check(st['sp']==sp,f'{name} sp');check(st['saved'],f'{name} callee-saved')
        lines=sorted(set(a for _,a in st['dc']))
        if ram:
            want=list(range(buf&~63,buf+size,64))
            check(lines==want,f'{name} {hex(buf)} lines {len(lines)} want {len(want)}')
            ops={o for o,_ in st['dc']};check(ops=={'civac'} if entry==READ else ops=={'cvac'},f'{name} ops {ops}')
            if entry==READ:check(len(st['dc'])==2*len(want),f'read before+after')
        else:check(not st['dc'],f'{name} {hex(buf)} skipped')
print('checks',n,'all_pass' if ok else 'FAILED')
