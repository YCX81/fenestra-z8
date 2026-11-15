"""Replay actual v51d MOV parser/FPS selector/time conversion; no native UI claim."""
from pathlib import Path
import struct,subprocess,tempfile,json,hashlib,sys
H=Path(__file__).resolve().parent;L=H.parents[1]
def text_section(b):
 shoff=struct.unpack_from('<Q',b,40)[0];shent,shnum,shstr=struct.unpack_from('<HHH',b,58)
 def hdr(i):return struct.unpack_from('<IIQQQQIIQQ',b,shoff+i*shent)
 ss=hdr(shstr);names=b[ss[4]:ss[4]+ss[5]]
 for i in range(shnum):
  s=hdr(i);name=names[s[0]:].split(b'\0',1)[0]
  assert name!=b'.rela.text'
  if name==b'.text':out=b[s[4]:s[4]+s[5]]
 return out
fw=H/'v51d.main';assert hashlib.sha256(fw.read_bytes()).hexdigest()=='aa6e94e0042fb445c666de73c0f6b9e7e569010b7993a3ac3da0979b174a9433'
source=(L/'feat-mov/evidence/ncdt-replay.S').read_text()
anchor='    cbnz w25, next_movie\n    ldr x0, =0x4e002000\n';assert source.count(anchor)==1
source=source.replace(anchor,'''    cbnz w25, next_movie
    ldr x0, =0x4e002000
    ldr x0, [x0]
    ldr x1, =0x4e003000
    add x2, x1, #4
    ldr x16, =0x40ce7dc0
    blr x16
    bl print_word
    ldr x8, =0x4e003000
    ldr w0, [x8]
    bl print_word
    ldr x8, =0x4e003000
    ldr w0, [x8, #4]
    bl print_word
    // Populate the two fields copied by stock callback 40cfc090 from its
    // successful-open payload +14/+18. No parser bytes are modified.
    ldr x8, =0x4e003000
    ldp w9, w10, [x8]
    ldr x8, =0xa0319f284
    stp w9, w10, [x8]
    ldr x8, =0xa0319b390
    mov w9, #1
    str w9, [x8]
    // Native display-rate selector from the exact parser ratio.
    ldr x8, =0x4e003000
    ldp w1, w0, [x8]
    ldr x16, =0x40d02bec
    blr x16
    bl print_word
    mov w21, #0
frame_loop:
    adr x8, frame_indices
    ldr w0, [x8, x21, lsl #2]
    ldr x1, =0x4e003010
    ldr x16, =0x40cf4114
    blr x16
    ldr x8, =0x4e003010
    ldr w0, [x8]
    bl print_word
    add w21, w21, #1
    cmp w21, #6
    b.lo frame_loop
    // Standard CFR control, through identical native getter/time functions.
    ldr x8, =0xa0319f284
    mov w9, #1001
    mov w10, #30000
    stp w9, w10, [x8]
    mov w0, #30000
    mov w1, #1001
    ldr x16, =0x40d02bec
    blr x16
    bl print_word
    mov w0, #77
    ldr x1, =0x4e003010
    ldr x16, =0x40cf4114
    blr x16
    ldr x8, =0x4e003010
    ldr w0, [x8]
    bl print_word
    b next_movie
frame_indices: .word 0,1,15,30,60,77
    ldr x0, =0x4e002000
''')
path=Path(sys.argv[1]) if len(sys.argv)>1 else L/'feat-v50/audio/qemu-v51d/runs/v51d-final-live/files/LIVE/100NCZ_8/DSC_0001.MOV'
blob=path.read_bytes()
def boxes(lo,hi):
 while lo<hi:
  n,t=struct.unpack_from('>I4s',blob,lo);assert n>=8 and lo+n<=hi
  yield t,lo,n;lo+=n
moov=next((p,n) for t,p,n in boxes(0,len(blob)) if t==b'moov')
for t,p,n in boxes(moov[0]+8,sum(moov)):
 if t!=b'trak':continue
 md=next((p,n) for t,p,n in boxes(p+8,p+n) if t==b'mdia');ch={t:(p,n) for t,p,n in boxes(md[0]+8,sum(md))}
 if blob[ch[b'hdlr'][0]+16:ch[b'hdlr'][0]+20]!=b'vide':continue
 scale,duration=struct.unpack_from('>II',blob,ch[b'mdhd'][0]+20);mn=ch[b'minf'];st=next((p,n) for t,p,n in boxes(mn[0]+8,sum(mn)) if t==b'stbl');tb={t:(p,n) for t,p,n in boxes(st[0]+8,sum(st))}
 frames=struct.unpack_from('>I',blob,tb[b'stsz'][0]+16)[0];delta=struct.unpack_from('>I',blob,tb[b'stts'][0]+20)[0]
indices=[0,1,15,30,60,frames]
source=source.replace('frame_indices: .word 0,1,15,30,60,77','frame_indices: .word '+','.join(map(str,indices)))
packet=bytes(4)+struct.pack('<I',len(blob))+blob+bytes(-len(blob)%4)+bytes(4)
with tempfile.TemporaryDirectory(prefix='native-time-',dir=H) as td:
 t=Path(td);subprocess.run(['clang','--target=aarch64-none-elf','-x','assembler','-c','-o',str(t/'probe.o'),'-'],input=source.encode(),check=True)
 (t/'probe.bin').write_bytes(text_section((t/'probe.o').read_bytes()));(t/'inputs.bin').write_bytes(packet)
 cmd=['qemu-system-aarch64','-M','virt','-cpu','cortex-a57','-accel','tcg','-m','1280M','-display','none','-nodefaults','-nic','none','-semihosting-config','enable=on,target=native']
 for p,a in [(fw,0x40160000),(t/'inputs.bin',0x48000000),(t/'probe.bin',0x47000000)]:cmd+=['-device',f'loader,file={p},addr={a:#x},force-raw=on'+(',cpu-num=0' if a==0x47000000 else '')]
 output=subprocess.check_output(cmd,stderr=subprocess.STDOUT,timeout=30).decode()
words=[int(x,16) for x in output.splitlines()]
expected=[0,0,delta,scale,2]+[i*delta*1000//scale for i in indices]+[2,2569]
assert words==expected,(words,expected)
report={'status':'PASS','scope':'actual C3.11 parser/getter/display-rate-selector/time-conversion under QEMU virt; open-callback copy modeled; no screen or pacing simulation','firmware_sha256':hashlib.sha256(fw.read_bytes()).hexdigest(),'input_sha256':hashlib.sha256(blob).hexdigest(),'parser_period_numerator':words[2],'parser_period_denominator':words[3],'stock_display_rate_enum':words[4],'frame_indices':indices,'stock_milliseconds':words[5:11],'cfr_control_enum':words[11],'cfr_control_77_frames_ms':words[12],'video_mdhd_duration':duration,'video_mdhd_scale':scale}
(H/(sys.argv[2] if len(sys.argv)>2 else 'native-timing-test.json')).write_text(json.dumps(report,indent=2)+'\n');(H/'native-timing-probe.S').write_text(source);print(json.dumps(report,indent=2))
