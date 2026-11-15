from pathlib import Path
import ast,subprocess,json,hashlib
from elftools.elf.elffile import ELFFile
R=Path('/path/to/user/fxfix/v53-evf/diag');G=Path('/path/to/user/v37build/v52g');S=G/'work/src-fix37'
flags=['--target=aarch64-none-elf','-Os','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-ffunction-sections','-fdata-sections','-mgeneral-regs-only','-mstrict-align','-DLA_WIDTH=1024','-DLA_HEIGHT=680','-DLA_RING_CAPACITY=96','-DLA_SAMPLE_INTERVAL_US=25000','-DLIVE_BITRATE=8000000','-DLIVE_IDR_US=500000','-DLIVE_ENCODE_SPEED=9','-Wall','-Wextra','-Werror','-I',str(R),'-I',str(S),'-I',str(S/'ui'),'-I',str(S/'ui/core/include'),'-I',str(S/'codec/freestanding'),'-include',str(R/'proto.h')]
def run(args):subprocess.run(list(map(str,args)),check=True)
def sections(p):
 with p.open('rb') as f:return {s.name:s.data() for s in ELFFile(f).iter_sections() if s['sh_flags']&2}
for name,oldname in [('live_ui','ui/live_ui.c'),('runtime','runtime.c')]:
 run(['clang-21',*flags,'-c',R/(name+'.c'),'-o',R/(name+'.o')])
 run(['clang-21',*flags,'-c',S/oldname,'-o',R/(name+'_base.o')])
 base=G/'live/objects'/(oldname.replace('/','_')+'.o')
 assert sections(R/(name+'_base.o'))==sections(base),(name,'baseline compiler mismatch')
names=next(ast.literal_eval(n.value) for n in ast.walk(ast.parse((G/'relive_v37.py').read_text())) if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='names' for t in n.targets))
objs=[R/'runtime.o' if n=='runtime.c' else R/'live_ui.o' if n=='ui/live_ui.c' else G/'live/objects'/(n.replace('/','_')+'.o') for n in names]
run(['ld.lld-21','-T',G/'live/payload.ld',*objs,'-o',R/'payload.elf'])
with (R/'payload.elf').open('rb') as f:
 e=ELFFile(f);syms={s.name:s['st_value'] for s in e.get_section_by_name('.symtab').iter_symbols()};ss={s.name:(s['sh_addr'],s.data()) for s in e.iter_sections() if s['sh_flags']&2 and s['sh_size']}
(R/'symbols.json').write_text(json.dumps(syms,indent=2)+'\n')
main=max(a+len(b) for n,(a,b) in ss.items() if n!='.live_extra')-0x401e2000
extra=len(ss['.live_extra'][1]);res=dict(main_bytes=main,main_free=0x401fd000-0x401e2000-main,extra_bytes=extra,extra_free=0x401fe000-0x401fd218-extra,baseline_object_match=True,elf_sha256=hashlib.sha256((R/'payload.elf').read_bytes()).hexdigest(),sections={n:[hex(a),len(b)] for n,(a,b) in ss.items()})
(R/'link-result.json').write_text(json.dumps(res,indent=2)+'\n');print(json.dumps(res))
