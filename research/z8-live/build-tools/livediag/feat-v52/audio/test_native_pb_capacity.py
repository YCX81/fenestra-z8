"""Execute original fixed-buffer init and playback-buffer handler on QEMU virt.

Only RTOS locks/event waits and queue delivery are replaced with synchronous
test shims. Native allocator, service 1a, native 109, and callbacks are executed.
This proves capacity/parameter propagation, not audible hardware behavior.
"""
from pathlib import Path
import hashlib,io,json,struct,subprocess,tempfile
from elftools.elf.elffile import ELFFile
HERE=Path(__file__).resolve().parent;LIVE=HERE.parents[1]
FW=LIVE/'feat-v50/integration-writer/v51d/q/main.main'
original=(LIVE/'feat-mov/evidence/ncdt-replay.S').read_text()
prefix=original.split("    // Initialize the guest's table")[0]
tail=original[original.index('finish:\n'):]
asm=prefix+'''
    ldr x8, =0xa0497d028
    ldr x9, =0x4d000000
    str x9, [x8]
    ldr x16, =0x40d24148
    blr x16
    ldr x8, =0xa031f8bb8
    ldr w0, [x8]
    bl print_word
    ldr x8, =0xa031f8bb8
    ldr w0, [x8, #8]
    bl print_word
    ldr x8, =0xa031f8bb8
    ldr w0, [x8, #12]
    bl print_word
    ldr x8, =0xa031f8bb8
    ldr w0, [x8, #16]
    bl print_word
    ldr x8, =0xa0497d028
    ldr w0, [x8]
    bl print_word
    ldr x8, =0xa031f8d44
    mov w9, #1
    strb w9, [x8]
    ldr x8, =0xa02d4cb48
    str w9, [x8]
    ldr x19, =0x4e000000
    adr x8, callback
    str x8, [x19, #0x18]
    ldr w8, =0x4318001a
    str w8, [x19, #0x20]
    ldr x20, =0x4e001000
    str x20, [x19, #0x28]
    ldr x8, =0x4d000000
    str x8, [x20]
    ldr w8, =960000
    str w8, [x20, #8]
    mov w8, #1
    str w8, [x20, #12]
    strb w8, [x20, #20]
    mov x0, x19
    ldr x16, =0x40d250cc
    blr x16
    ldr w0, =0xdeadbeef
    bl print_word
    ldr x8, =0xa02d4cbd8
    ldr w0, [x8]
    bl print_word
    ldr x8, =0xa02d4cbd8
    ldr w0, [x8, #4]
    bl print_word
    ldr x8, =0xa02d4cbd8
    ldr w0, [x8, #24]
    bl print_word
    ldr x8, =0x4e001000
    ldrb w0, [x8, #21]
    bl print_word
    b finish
callback:
    str x30, [sp, #-16]!
    bl print_word
    ldr x30, [sp], #16
    ret
queue_shim:
    str x30, [sp, #-16]!
    ldr x16, =0x40c81ec0
    blr x16
    mov w0, #1
    ldr x30, [sp], #16
    ret
'''+tail
with tempfile.TemporaryDirectory(dir=HERE,prefix='native-pb-') as td:
 t=Path(td);subprocess.run(['clang','--target=aarch64-none-elf','-x','assembler','-c','-o',str(t/'probe.o'),'-'],input=asm.encode(),check=True)
 e=ELFFile(io.BytesIO((t/'probe.o').read_bytes()));assert e.get_section_by_name('.rela.text') is None
 (t/'probe.bin').write_bytes(e.get_section_by_name('.text').data())
 symbols={s.name:s['st_value'] for s in e.get_section_by_name('.symtab').iter_symbols()}
 fw=bytearray(FW.read_bytes());stubs=[]
 for addr in (0x40d2c674,0x40d2c660,0x40d2bcc8,0x40d2bc8c,0x40d2bdf4):
  struct.pack_into('<II',fw,addr-0x40160000,0x52800020,0xd65f03c0);stubs.append(hex(addr))
 addr=0x40c80988;dest=0x47000000+symbols['queue_shim'];off=(dest-addr)//4
 struct.pack_into('<I',fw,addr-0x40160000,0x14000000|(off&0x3ffffff))
 (t/'shim.main').write_bytes(fw)
 cmd=['qemu-system-aarch64','-M','virt','-cpu','cortex-a57','-accel','tcg','-m','1280M','-display','none','-nodefaults','-nic','none','-semihosting-config','enable=on,target=native','-device',f'loader,file={t/"shim.main"},addr=0x40160000,force-raw=on','-device',f'loader,file={t/"probe.bin"},addr=0x47000000,force-raw=on,cpu-num=0']
 out=subprocess.check_output(cmd,stderr=subprocess.STDOUT,timeout=10).decode()
words=[int(x,16) for x in out.splitlines()]
assert words==[0x4d000000,1000000,200000,5,0x4d000000+1000000,0x4318001a,0xdeadbeef,1,960000,0x4d000000,1],words
report={'passed':True,'firmware_sha256':hashlib.sha256(FW.read_bytes()).hexdigest(),'native_capacity':1000000,'native_stride':200000,'native_count':5,'submitted_bytes':960000,'submitted_count':1,'service_reply':'0x4318001a','service_result':1,'native_buffer_bytes':words[-3],'native_buffer_ptr':hex(words[-2]),'rtos_shims':stubs,'post_shim':'0x40c80988 synchronous original dispatcher','limit':'No DAC or real DMA is exercised; actual source code and original ABI/capacity execute.'}
(HERE/'native-pb-capacity.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
