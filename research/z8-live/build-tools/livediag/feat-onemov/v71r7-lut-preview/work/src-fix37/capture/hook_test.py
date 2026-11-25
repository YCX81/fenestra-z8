"""Unicorn test of the actual linked AArch64 entry wrappers; never runs QEMU.
Notify callees are adversarial stubs, so this tests wrapper preservation only.
"""
import argparse
import hashlib
import json
from pathlib import Path
import random
import struct
import subprocess
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_NZCV
import unicorn.arm64_const as arm


def segments(blob):
    assert blob[:6] == b'\x7fELF\x02\x01'
    phoff = struct.unpack_from('<Q', blob, 32)[0]
    phsize, phcount = struct.unpack_from('<HH', blob, 54)
    for i in range(phcount):
        typ, flags, off, va, pa, filesz, memsz, align = struct.unpack_from('<II6Q', blob, phoff+i*phsize)
        if typ == 1 and memsz:
            yield va, memsz, blob[off:off+filesz]


def test(elf):
    blob = elf.read_bytes()
    nm = subprocess.check_output(['llvm-nm', '--defined-only', str(elf)], text=True)
    symbols = {p[2]: int(p[0],16) for line in nm.splitlines() if len(p:=line.split()) == 3}
    cases = []
    for entry, helper, resume, irq in [('live_irq_entry','live_irq_notify','live_irq_resume',True),
                                     ('live_press_entry','live_press_notify','live_old_movie_press',False)]:
        for flags in range(16):
            rng = random.Random(flags + (100 if irq else 200))
            uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
            pages = set()
            def map_range(address, length):
                for page in range(address & ~4095, (address+length+4095) & ~4095, 4096):
                    if page not in pages:
                        uc.mem_map(page,4096);pages.add(page)
            for va, size, contents in segments(blob):
                map_range(va,size);uc.mem_write(va,contents)
            map_range(symbols[resume],4)
            stack = 0x90000000
            map_range(stack-65536,65536)
            regs = [getattr(arm,f'UC_ARM64_REG_X{i}') for i in range(31)]
            before = [rng.getrandbits(64) for _ in regs]
            before[0],before[1] = 0x41070030,1
            for reg,val in zip(regs,before):uc.reg_write(reg,val)
            uc.reg_write(UC_ARM64_REG_SP,stack)
            uc.reg_write(UC_ARM64_REG_NZCV,flags<<28)
            called=[];reached=[]
            def instruction(cpu,address,size,opaque):
                if address == symbols[helper]:
                    called.append(address)
                    assert cpu.reg_read(regs[0]) == before[0]
                    assert cpu.reg_read(regs[1]) == before[1]
                    return_pc=cpu.reg_read(regs[30])
                    # More destructive than the AAPCS caller-clobbered set:
                    # ensure wrapper really protects all integer registers.
                    for reg in regs:cpu.reg_write(reg,rng.getrandbits(64))
                    cpu.reg_write(UC_ARM64_REG_NZCV,(flags^15)<<28)
                    cpu.reg_write(UC_ARM64_REG_PC,return_pc)
                elif address == symbols[resume]:
                    reached.append(address);cpu.emu_stop()
            uc.hook_add(UC_HOOK_CODE,instruction)
            uc.emu_start(symbols[entry],0,count=200)
            assert called == [symbols[helper]],(entry,called)
            assert reached == [symbols[resume]],(entry,reached)
            after=[uc.reg_read(reg) for reg in regs]
            assert before == after,(entry,[(i,a,b) for i,(a,b) in enumerate(zip(before,after)) if a!=b])
            assert uc.reg_read(UC_ARM64_REG_NZCV)==flags<<28
            expected_sp=stack-16 if irq else stack
            assert uc.reg_read(UC_ARM64_REG_SP)==expected_sp
            if irq:
                assert struct.unpack('<QQ',uc.mem_read(expected_sp,16)) == (before[19],before[30])
            cases.append(dict(entry=entry,nzcv=hex(flags<<28),all_x0_x30_preserved=True,
                              sp_matches_original_prologue=True,original_resume=hex(symbols[resume])))
    return dict(elf=str(elf),sha256=hashlib.sha256(blob).hexdigest(),case_count=len(cases),
                passed=True,notify_functions_stubbed=True,simd_executed=False,cases=cases,
                scope='Actual linked wrappers only; IRQ SP includes original stp x19,x30 prologue. No firmware service execution.')

if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--elf',type=Path,default=Path('/path/to/user/.local/share/nikon-z8-qemu/fenestra-z8/bundle/live-a/payload.elf'))
    parser.add_argument('--output',type=Path,default=Path(__file__).with_name('hook-test-results.json'))
    args=parser.parse_args();result=test(args.elf)
    args.output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k!='cases'},indent=2))
