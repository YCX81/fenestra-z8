"""Execute native stop message in two no-hardware driver states, including its callback ABI."""
from pathlib import Path
import hashlib,io,json,subprocess,tempfile
from elftools.elf.elffile import ELFFile
HERE=Path(__file__).resolve().parent;LIVE=HERE.parents[1]
FW=LIVE/'feat-v50/integration-writer/v51d/q/main.main'
original=(LIVE/'feat-mov/evidence/ncdt-replay.S').read_text()
prefix=original.split("    // Initialize the guest's table")[0]
tail=original[original.index('finish:\n'):]
asm=prefix+'''
    mov w24, #4
case_start:
    ldr x0, =0x4e000000
    mov w1, #0
    mov w2, #0x4000
    ldr x16, =0x4127ab40
    blr x16
    ldr x19, =0x4e000000
    adr x8, callback
    str x8, [x19, #0x18]
    ldr w8, =0x42040108
    str w8, [x19, #0x20]
    ldr x8, =0x4e003000
    str x8, [x19, #0x28]
    ldr x8, =0x4e002000
    str x8, [x19, #0x30]
    ldr x8, =0xa02d4cb48
    str w24, [x8]
    mov x0, x19
    ldr x16, =0x40c81ec0
    blr x16
    ldr x8, =0x4e002000
    ldr w0, [x8]
    bl print_word
    ldr x8, =0x4e002000
    ldr w0, [x8, #4]
    bl print_word
    ldr x8, =0x4e002000
    ldr w0, [x8, #8]
    bl print_word
    ldr x8, =0xa02d4cb48
    ldr w0, [x8]
    bl print_word
    ldr x8, =0x4e003000
    ldrb w0, [x8]
    bl print_word
    cmp w24, #4
    mov w24, #1
    b.eq case_start
    b finish
callback:
    str w0, [x1]
    str w1, [x1, #4]
    ldr x8, =0xa02d4cb48
    ldr w8, [x8]
    str w8, [x1, #8]
    ret
'''+tail
with tempfile.TemporaryDirectory(dir=HERE,prefix='native-stop-') as td:
 t=Path(td)
 subprocess.run(['clang','--target=aarch64-none-elf','-x','assembler','-c','-o',str(t/'probe.o'),'-'],input=asm.encode(),check=True)
 e=ELFFile(io.BytesIO((t/'probe.o').read_bytes()));assert e.get_section_by_name('.rela.text') is None
 (t/'probe.bin').write_bytes(e.get_section_by_name('.text').data())
 cmd=['qemu-system-aarch64','-M','virt','-cpu','cortex-a57','-accel','tcg','-m','1280M','-display','none','-nodefaults','-nic','none','-semihosting-config','enable=on,target=native','-device',f'loader,file={FW},addr=0x40160000,force-raw=on','-device',f'loader,file={t/"probe.bin"},addr=0x47000000,force-raw=on,cpu-num=0']
 out=subprocess.check_output(cmd,stderr=subprocess.STDOUT,timeout=10).decode()
words=[int(x,16) for x in out.splitlines()]
assert words==[0x42040108,0x4e002000,4,1,1,0x42040108,0x4e002000,1,1,0],words
report={'passed':True,'firmware_sha256':hashlib.sha256(FW.read_bytes()).hexdigest(),'callback_offset':'0x18','context_offset':'0x30','event':'0x42040108','cases':[{'initial_state':4,'callback_observed_state':4,'final_state':1,'payload_result':1},{'initial_state':1,'callback_observed_state':1,'final_state':1,'payload_result':0}],'note':'For state4 callback precedes idle publication, so callback alone cannot authorize release. Active state5 follows native asynchronous completion and is covered by source/mock timing, not physical DMA here.'}
(HERE/'native-stop-callback.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
