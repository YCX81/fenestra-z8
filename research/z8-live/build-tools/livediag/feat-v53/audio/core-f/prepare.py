from pathlib import Path
import re,subprocess,json,hashlib
from elftools.elf.elffile import ELFFile
P=Path(__file__).resolve().parent; D=P/'driver'; S=P.parents[1]/'integration-f/work/src-fix37'
c=D/'runtime-layout.c';text=(P.parents[1]/'integration-a/driver/runtime-layout.c').read_text().replace('\n};','');
for f in ['audio_warm','audio.first_index','audio.next_index','audio.marked','audio.frozen','audio_z8.head','audio_z8.tail','audio_z8.unmuted','audio_z8.anchor_us','audio_z8.frames','audio_z8.go','audio_z8.resume_bridge','shutter.phase','shutter.generation','shutter.captured','shutter.played','shutter.completed','shutter.cancelled','shutter.errors']:
 text+=f',\noffsetof(struct live_runtime,{f}),sizeof(((struct live_runtime*)0)->{f})'
c.write_text(text+'\n};\n')
subprocess.run(['clang','--target=aarch64-none-elf','-c','-I'+str(S),str(c),'-o',str(D/'runtime-layout.o')],check=True)
import struct
with (D/'runtime-layout.o').open('rb') as fp: data=ELFFile(fp).get_section_by_name('.rodata').data()
v=struct.unpack('<'+'I'*(len(data)//4),data);assert v[0]==23744,v[0]
fields=re.findall(r'offsetof\(struct live_runtime,([^)]*)\)',text);assert len(v)==1+len(fields)*2
(D/'runtime-offsets.json').write_text(json.dumps(dict(struct_bytes=v[0],**dict(zip(fields,v[1::2]))),indent=2)+'\n')
(D/'runtime-widths.json').write_text(json.dumps(dict(zip(fields,v[2::2])),indent=2)+'\n')
r=D/'retry-driver/run_live.py';t=r.read_text()
t=t.replace("def sha(path):", "EXPECTED_MAIN='b0cd7c40c3c6ffdf07b87fcbab20b98fbe9ed0c2a848550855d989b9e64fd8b0'\ndef sha(path):")
t=t.replace("self.command[self.command.index('-kernel')+1]=str(A.main.resolve())", "assert sha(A.main)==EXPECTED_MAIN, 'exact F MAIN mismatch'\n        self.command[self.command.index('-kernel')+1]=str(A.main.resolve())")
t=t.replace("for prop in ['runtime-clock-state'", "for prop in ['serial5-pcm-state','lpcm-state','serial5-audio-state','runtime-clock-state'")
t=t.replace("extra+=['z8-serial5-config-model=on'", "extra+=['serial5-audio-config-model=on','lpcm-host-conversion-model=on','lpcm-ram-aperture-fixture=on','z8-serial5-config-model=on'")
t=t.replace("shutter('post-encode-30s',gap=30)","shutter('post-encode-4s',gap=4)").replace("shutter('post-complete',gap=30)","shutter('post-complete',gap=4)")
t=t.replace("s['runtime']=runtime", "s['runtime']=runtime\n    s['native_audio']={'driver':rd(0xa02d4cb48,4),'serial':g.read_bytes(0x0e200000,0x400).hex(),'pdma':g.read_bytes(0x0d100000,0x400).hex()}" )
# g.read_bytes cannot safely assume physical MMIO alias maps; instead rely on passive observer and QOM.
t=t.replace("s['native_audio']={'driver':rd(0xa02d4cb48,4),'serial':g.read_bytes(0x0e200000,0x400).hex(),'pdma':g.read_bytes(0x0d100000,0x400).hex()}","s['native_audio']={'driver':rd(0xa02d4cb48,4)}")
t=t.replace("self.command += ['-plugin',f'{plugin},out={self.out}/lv-observer.dat,{options}']", "self.command += ['-plugin',f'{plugin},out={self.out}/lv-observer.dat,{options}']\n        self.command += ['-plugin',f'{TASK}/rx_observer.so,out={self.out}/rx.jsonl']")
r.write_text(t)
# Fix valid-x0 NULL handle in own copy only.
c=D/'lv_observer.c';t=c.read_text().replace('static GMutex mutex;', 'static GMutex mutex;static unsigned found[4][5];')
t=t.replace('if(regs[cpu][i]&&qemu_plugin_read_register','if(found[cpu][i]&&qemu_plugin_read_register')
t=t.replace('regs[cpu][j]=d->handle;', '{regs[cpu][j]=d->handle;found[cpu][j]=1;}')
c.write_text(t)
print('layout',v[0],len(fields));print(S)
