"""Replay actual MOV-open and 0x40ce6c84 native GOP/chunk checks in QEMU virt."""
from pathlib import Path
import sys,struct,subprocess,io,json,hashlib
from elftools.elf.elffile import ELFFile
H=Path(__file__).resolve().parent;OLD=H.parents[1]/'feat-mov';sys.path.insert(0,str(OLD))
import mov_fix
asm=(OLD/'evidence/ncdt-replay.S').read_text();a=asm.index('#ifdef PROBE_HEVC_HEADERS');b=asm.index('seek_absolute:')
asm=asm[:a]+'''    ldr x16, =0x40ce6c84
    blr x16
    bl print_word
    b next_movie
'''+asm[b:]
D=H/'parser';(D/'gop.S').write_text(asm)
subprocess.run(['clang','--target=aarch64-none-elf','-c',str(D/'gop.S'),'-o',str(D/'gop.o')],check=True)
elf=ELFFile(io.BytesIO((D/'gop.o').read_bytes()));assert elf.get_section_by_name('.rela.text') is None
(D/'gop.bin').write_bytes(elf.get_section_by_name('.text').data())
good=(H/'host-sample.MOV').read_bytes()
bad=mov_fix.rewrite((H.parents[1]/'feat-w5dec/hw-DSC_0028.MOV').read_bytes(),niko=True,audio_stco=True)
cases=[('C writer GOP chunks',good,[0,0]),('old per-frame chunks rejected',bad,[0,6])]
p=bytearray(4)
for _,d,_ in cases:p+=struct.pack('<I',len(d))+d+bytes(-len(d)%4)
p+=bytes(4);(D/'gop-inputs.bin').write_bytes(p)
cmd=['qemu-system-aarch64','-M','virt','-cpu','cortex-a57','-accel','tcg','-m','1280M','-display','none','-nodefaults','-nic','none','-semihosting-config','enable=on,target=native','-device',f'loader,file={D}/bg34.main,addr=0x40160000,force-raw=on','-device',f'loader,file={D}/gop-inputs.bin,addr=0x48000000,force-raw=on','-device',f'loader,file={D}/gop.bin,addr=0x47000000,force-raw=on,cpu-num=0']
r=subprocess.run(cmd,check=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=20)
words=[int(x,16) for x in r.stdout.decode().splitlines()];assert len(words)==4
out=[]
for i,(name,d,want) in enumerate(cases):
 got=words[2*i:2*i+2];print(name,got);assert got==want
 out.append(dict(name=name,sha256=hashlib.sha256(d).hexdigest(),actual=got))
(D/'gop-parser-test.json').write_text(json.dumps(dict(status='PASS',cases=out,scope='actual firmware instructions with file adapter stubs in QEMU virt'),indent=2)+'\n')
