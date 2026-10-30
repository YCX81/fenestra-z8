"""Diagnostic: record the first FX_CAPABILITY origin (file tag, line) in the scratch page; restores sources after build."""
import re,subprocess
from pathlib import Path
R=Path('..')
FILES={1:'src/platform/z8_c311/module_session.c',2:'src/platform/z8_c311/module_worker.c',3:'src/platform/z8_c311/address_space.c',
       4:'src/platform/z8_c311/exec_context.c',5:'src/loader.c',6:'src/exec.c',7:'src/module.c',8:'src/runtime.c',9:'src/native_modules.c',10:'src/module_manager.c'}
saved={}
MAC='#define FX_BC(t,l) (*(volatile uint32_t *)(uintptr_t)UINT64_C(0xa1172a180)==0u?(*(volatile uint32_t *)(uintptr_t)UINT64_C(0xa1172a180)=((t)<<16)|(l)):0u)\n'
for t,f in FILES.items():
    p=R/f;b=p.read_bytes();saved[p]=b;s=b.decode().replace('\r\n','\n');out=[]
    for n,l in enumerate(s.split('\n'),1):
        if not l.lstrip().startswith('#'):
            l=re.sub(r'\bFX_CAPABILITY\b',f'(FX_BC({t},{n}),FX_CAPABILITY)',l)
        out.append(l)
    p.write_text(MAC+'\n'.join(out),encoding='utf-8',newline='\n')
try:
    r=subprocess.run(['python','diagbuild.py','0xa1172a000'],capture_output=True,text=True,stdin=subprocess.DEVNULL)
    print(r.stdout[-1200:],r.stderr[-500:])
finally:
    for p,b in saved.items():p.write_bytes(b)
