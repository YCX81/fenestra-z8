"""Run the original fire-event gate: distinguish event/state filtering from DAC."""
from pathlib import Path
import hashlib,io,json,struct,subprocess,tempfile
from elftools.elf.elffile import ELFFile
H=Path(__file__).resolve().parent;LIVE=H.parents[1];FW=LIVE/'feat-v50/integration-writer/v51d/q/main.main'
old=(LIVE/'feat-mov/evidence/ncdt-replay.S').read_text()
cases=[(12,0,0,1),(12,0,0,5),(12,1,0,5),(12,0,1,5),(11,0,0,5),(0,0,0,1)]
asm=old.split("    // Initialize the guest's table")[0]+'''
    adr x24, cases
    mov w25, #6
again:
    ldp w9, w10, [x24]
    ldp w11, w12, [x24, #8]
    ldr x8, =0xa0292be1c
    str w9, [x8]
    ldr x8, =0xa03bc7008
    strb w10, [x8]
    str wzr, [x8, #8]
    strb w11, [x8, #14]
    ldr x8, =0xa02d4cb48
    str w12, [x8]
    ldr x8, =0x4e000000
    str xzr, [x8]
    mov w0, #4
    ldr x16, =0x40f9c5cc
    blr x16
    ldr x8, =0x4e000000
    ldr w0, [x8]
    bl print_word
    ldr x8, =0x4e000000
    ldr w0, [x8, #4]
    bl print_word
    add x24, x24, #16
    subs w25, w25, #1
    b.ne again
    b finish
gate:
    stp x9,x16,[sp,#-16]!
    ldr x16, =0x4e000000
    ldr w9,[x16]
    add w9,w9,#1
    str w9,[x16]
    ldp x9,x16,[sp],#16
    ldrb w8,[x8]
    ret
sound:
    stp x9,x16,[sp,#-16]!
    ldr x16, =0x4e000000
    add w9,w0,#1
    str w9,[x16,#4]
    ldp x9,x16,[sp],#16
    ret
cases:
'''+''.join('    .word '+','.join(map(str,c))+'\n' for c in cases)+old[old.index('finish:\n'):]
with tempfile.TemporaryDirectory(dir=H,prefix='sound-gate-') as td:
 t=Path(td);subprocess.run(['clang','--target=aarch64-none-elf','-x','assembler','-c','-o',str(t/'p.o'),'-'],input=asm.encode(),check=True)
 e=ELFFile(io.BytesIO((t/'p.o').read_bytes()));assert not e.get_section_by_name('.rela.text')
 (t/'p.bin').write_bytes(e.get_section_by_name('.text').data());sym={x.name:0x47000000+x['st_value'] for x in e.get_section_by_name('.symtab').iter_symbols()}
 fw=bytearray(FW.read_bytes())
 for at,label,call in [(0x40f9cb64,'gate',True),(0x40f9ce60,'sound',False)]:
  delta=sym[label]-at;struct.pack_into('<I',fw,at-0x40160000,(0x94000000 if call else 0x14000000)|((delta//4)&0x3ffffff))
 for at in (0x40f9de20,0x40d2bcc8):struct.pack_into('<I',fw,at-0x40160000,0xd65f03c0)
 (t/'main').write_bytes(fw)
 cmd=['qemu-system-aarch64','-M','virt','-cpu','cortex-a57','-accel','tcg','-m','1280M','-display','none','-nodefaults','-nic','none','-semihosting-config','enable=on,target=native','-device',f'loader,file={t/"main"},addr=0x40160000,force-raw=on','-device',f'loader,file={t/"p.bin"},addr=0x47000000,force-raw=on,cpu-num=0']
 out=subprocess.check_output(cmd,stderr=subprocess.STDOUT,timeout=10).decode()
words=[int(x,16) for x in out.splitlines()];assert words==[1,13,1,13,1,0,1,0,0,0,0,0],words
rows=[dict(state=c[0],mute=c[1],inhibit=c[2],driver=c[3],mute_gate_hits=words[2*i],sound_id=words[2*i+1]-1) for i,c in enumerate(cases)]
report={'passed':True,'firmware_sha256':hashlib.sha256(FW.read_bytes()).hexdigest(),'cases':rows,'scope':'Original 40f9c5cc fire-event instructions, instrumented existing mute hook; sound/DAC entry, logger and RTOS event-clear stubbed. Does not prove producer event delivery or prepare completion.','conclusion':'Driver=5 alone does not prevent the mute hook when fire event4/state12 arrive; missing hook evidence must be earlier event delivery/state/prepare progress, not the downstream stock mute byte.'}
(H/'native-shutter-gate.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
