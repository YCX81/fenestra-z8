"""Actual v51d ARM audio handlers: validate payload output extents with canaries."""
from pathlib import Path
import hashlib,io,json,struct,subprocess,tempfile,sys
from elftools.elf.elffile import ELFFile
HERE=Path(__file__).resolve().parent
LIVE=HERE.parents[1]
WORK=Path(sys.argv[1]) if len(sys.argv)>1 else HERE/'baseline'
FW=LIVE/'feat-v50/integration-writer/v51d/q/main.main'
assert hashlib.sha256(FW.read_bytes()).hexdigest()=='aa6e94e0042fb445c666de73c0f6b9e7e569010b7993a3ac3da0979b174a9433'
source='#include <stddef.h>\n#include "audio/live_audio_z8.h"\nconst unsigned long offsets[]={offsetof(struct live_audio_z8,format),offsetof(struct live_audio_z8,channels),offsetof(struct live_audio_z8,ring),offsetof(struct live_audio_z8,stage),sizeof(((struct live_audio_z8*)0)->format),sizeof(((struct live_audio_z8*)0)->ring),sizeof(struct live_audio_z8)};\n'
original=(LIVE/'feat-mov/evidence/ncdt-replay.S').read_text()
prefix=original.split("    // Initialize the guest's table")[0]
tail=original[original.index('finish:\n'):]
with tempfile.TemporaryDirectory(dir=HERE,prefix='payload-') as td:
 t=Path(td)
 subprocess.run(['clang','--target=aarch64-none-elf','-ffreestanding','-I',str(WORK/'src-fix37'),'-x','c','-c','-o',str(t/'layout.o'),'-'],input=source.encode(),check=True)
 e=ELFFile(io.BytesIO((t/'layout.o').read_bytes()));layout=struct.unpack('<7Q',e.get_section_by_name('.rodata').data());fmt,ch,ring,stage,fmtbytes,ringbytes,size=layout
 asm=prefix+f'''
    ldr x0, =0x4e000000
    mov w1, #0
    mov w2, #4096
    ldr x16, =0x4127ab40
    blr x16
    ldr x19, =0x4e000000
    ldr x20, =0x4e001000
    ldr w8, =0x42040104
    str w8, [x20, #0x20]
    add x8, x19, #{fmt}
    str x8, [x20, #0x28]
    mov w8, #0xa5
    strb w8, [x19, #{ch}]
    ldr x8, =0xa02d4cb48
    mov w9, #2
    str w9, [x8]
    mov x0, x20
    ldr x16, =0x40c81ec0
    blr x16
    ldrb w0, [x19, #{ch}]
    bl print_word
    ldrb w0, [x19, #{fmt+0x20}]
    bl print_word
    ldr w8, =0x42040109
    str w8, [x20, #0x20]
    add x8, x19, #{ring}
    str x8, [x20, #0x28]
    mov w9, #5
    str w9, [x8]
    ldr w9, =96000
    str w9, [x8, #4]
    ldr w9, =24000
    str w9, [x8, #0xc]
    mov w9, #1
    strb w9, [x8, #0x10]
    ldr x9, =0x4e008000
    str x9, [x19, #{stage}]
    ldr x8, =0xa02d4cb48
    mov w9, #1
    str w9, [x8]
    mov x0, x20
    ldr x16, =0x40c81ec0
    blr x16
    ldr w0, [x19, #{stage}]
    bl print_word
    ldrb w0, [x19, #{ring+0x30}]
    bl print_word
    b finish
'''+tail
 subprocess.run(['clang','--target=aarch64-none-elf','-x','assembler','-c','-o',str(t/'probe.o'),'-'],input=asm.encode(),check=True)
 e=ELFFile(io.BytesIO((t/'probe.o').read_bytes()));assert e.get_section_by_name('.rela.text') is None
 (t/'probe.bin').write_bytes(e.get_section_by_name('.text').data())
 cmd=['qemu-system-aarch64','-M','virt','-cpu','cortex-a57','-accel','tcg','-m','1280M','-display','none','-nodefaults','-nic','none','-semihosting-config','enable=on,target=native','-device',f'loader,file={FW},addr=0x40160000,force-raw=on','-device',f'loader,file={t/"probe.bin"},addr=0x47000000,force-raw=on,cpu-num=0']
 out=subprocess.check_output(cmd,stderr=subprocess.STDOUT,timeout=10).decode()
words=[int(x,16) for x in out.splitlines()]
old=fmtbytes==0x20 and ringbytes==0x30
assert words==([0,0,0x4e008001,1] if old else [0xa5,0,0x4e008000,1]),words
report={'firmware_sha256':hashlib.sha256(FW.read_bytes()).hexdigest(),'source':str(WORK),'layout':dict(zip(['format','channels','ring','stage','format_bytes','ring_bytes','struct_bytes'],layout)),'scope':'Unmodified original ARM dispatcher, format failure-state 2 and ring state 1; RAM-only driver globals','channels_canary_before':0xa5,'channels_after':words[0],'format_reply_at_0x20':words[1],'stage_before':'0x4e008000','stage_after':hex(words[2]),'ring_reply_at_0x30':words[3],'old_overwrite_reproduced':old,'passed':True}
(HERE/('payload-old.json' if old else 'payload-fixed.json')).write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
