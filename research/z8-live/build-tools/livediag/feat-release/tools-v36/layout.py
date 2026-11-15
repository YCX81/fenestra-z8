from pathlib import Path
import subprocess,struct,io,json
from elftools.elf.elffile import ELFFile
R=Path(__file__).resolve().parent.parent;O=R/'audit-v36/layout';O.mkdir(parents=True,exist_ok=True)
flags='--target=aarch64-none-elf -Os -ffreestanding -fno-builtin -mgeneral-regs-only -mstrict-align -Wall -Wextra -Werror'.split()
def symbol(p,name):
 e=ELFFile(io.BytesIO(p.read_bytes()));s=next(s for s in e.get_section_by_name('.symtab').iter_symbols() if s.name==name)
 b=e.get_section(s['st_shndx']).data()[s['st_value']:s['st_value']+s['st_size']];return list(struct.unpack('<'+'I'*(len(b)//4),b))
results={}
for v in ('32','36'):
 src=R/('src-fix'+v);pb=R/('pbfix/live-playback-ui-icon' if v=='32' else 'pb-fix36')
 c=O/('src'+v+'.c');o=c.with_suffix('.o');lock='log_pad' if v=='32' else 'w5_close_lock';tail='staging_scratch' if v=='32' else 'rmw'
 c.write_text('#include "runtime.h"\n#include "codec/live_w5enc.c"\nconst unsigned layout[]={sizeof(struct live_runtime),offsetof(struct live_runtime,'+lock+'),sizeof(struct live_w5),offsetof(struct live_w5,'+tail+'),offsetof(struct live_runtime,stream),offsetof(struct live_runtime,capture)};\n')
 inc=['-I',str(src),'-I',str(src/'codec/freestanding'),'-I',str(src/'ui/core/include')]
 subprocess.run(['clang',*flags,*inc,'-c',str(c),'-o',str(o)],check=True)
 results[v]={'runtime_w5':symbol(o,'layout')}
 # Compare every runtime field offset using clang's record layout, not host ABI.
 dump=subprocess.check_output(['clang',*flags,*inc,'-Xclang','-fdump-record-layouts','-c',str(c),'-o',str(o)],text=True)
 (O/('record-layout-'+v+'.txt')).write_text(dump)
 ui=O/('ui'+v+'.o');subprocess.run(['clang',*flags,*inc,'-c',str(src/'ui/live_ui.c'),'-o',str(ui)],check=True)
 results[v]['live_ui_metadata']=symbol(ui,'live_ui_metadata')
 if v=='36':results[v]['icon32_metadata']=symbol(ui,'icon32_metadata')
 c=O/('pb'+v+'.c');o=c.with_suffix('.o')
 c.write_text('#include "runtime.h"\n#include <stddef.h>\nconst unsigned layout[]={sizeof(PbRuntime),offsetof(PbRuntime,display),offsetof(PbRuntime,gate),offsetof(PbRuntime,render_request)};\n')
 subprocess.run(['clang',*flags,'-I',str(pb),'-I',str(pb/'core-include'),'-c',str(c),'-o',str(o)],check=True)
 results[v]['pb']=symbol(o,'layout')
a,b=[results[v]['runtime_w5'] for v in ('32','36')];assert a[:3]==b[:3] and a[4:]==b[4:] and a[3]+4==b[3]
a,b=[results[v]['live_ui_metadata'] for v in ('32','36')];assert a[3:]==b[3:]
assert results['32']['pb'][1:]==results['36']['pb'][1:]
# Full nested runtime record, normalized only the renamed padding field.
def runtime(v):
 t=(O/('record-layout-'+v+'.txt')).read_text();a=t.index('         0 | struct live_runtime\n');b=t.index('*** Dumping',a) if '*** Dumping' in t[a:] else len(t)
 return t[a:b].replace('w5_close_lock','log_pad').replace('src-fix'+v,'src-fixXX').strip()
assert runtime('32')==runtime('36')
(O/'results.json').write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(results,indent=2))
