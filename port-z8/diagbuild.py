"""Temporary diagnostic variant: breadcrumbs in the lab scratch page. Restores menu.c afterwards."""
import os,re,shlex,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
MAIN=Path(os.environ['FENESTRA_Z8_MAIN']).expanduser().resolve()
def wsl_path(path):
    value=path.as_posix()
    return '/mnt/'+value[0].lower()+value[2:] if path.drive else value
M=ROOT/'src/platform/z8_c311/menu.c'; orig=M.read_bytes(); t=orig.decode()
SCR=sys.argv[1]
head=f'#define BC_BASE ((volatile uint32_t *)(uintptr_t)UINT64_C({SCR}))\n#define BC(n) (BC_BASE[1]=(uint32_t)(n))\n'
t=t.replace('#define FN(a, t)',head+'#define FN(a, t)',1)
a=t.index('static void install(void) {');b=t.index('\n}\n',a)
body=t[a:b]; L=t[:a].count('\n')
out=[]
for i,l in enumerate(body.split('\n')):
    out.append(re.sub(r'\breturn;',f'{{BC({L+i+1});return;}}',l))
t=t[:a]+'\n'.join(out)+f'\n    BC(1);'+t[b:]
t=t.replace('    uint64_t r = fx_z8_system_original(message, sync);','    BC_BASE[0]=BC_BASE[0]+1u;if(event<64u)BC_BASE[2u+event]=BC_BASE[2u+event]+1u;\n    uint64_t r = fx_z8_system_original(message, sync);',1)
t=t.replace('    (void)fx_startup_service_request(&s->startup,FX_START_ACTIVATE);','    BC_BASE[70]=BC_BASE[70]+1u;BC_BASE[71]=(uint32_t)fx_startup_service_request(&s->startup,FX_START_ACTIVATE);',1)
t=t.replace('static FxResult start_modules(void *context) {\n    State *s=context;','static FxResult start_modules(void *context) {\n    State *s=context;BC_BASE[72]=BC_BASE[72]+1u;',1)
t=t.replace('        fx_startup_service_tick(&s->startup);','        fx_startup_service_tick(&s->startup);BC_BASE[73]=BC_BASE[73]+1u;BC_BASE[74]=atomic_load(&s->startup.phase);BC_BASE[75]=atomic_load(&s->startup.error);BC_BASE[76]=atomic_load(&s->phase);BC_BASE[77]=atomic_load(&s->error);BC_BASE[78]=atomic_load(&s->activation);BC_BASE[79]=s->startup_files.native_error;',1)
M.write_text(t,encoding='utf-8',newline='\n')
try:
    command='cd '+shlex.quote(wsl_path(ROOT))+' && rm -rf build/z8-diag && python3 host/build_z8_core.py --firmware '+shlex.quote(wsl_path(MAIN))+' --output-dir build/z8-diag 2>&1 | tail -6 | cut -c1-400'
    r=subprocess.run(['wsl','bash','-c',command],capture_output=True,text=True)
    print(r.stdout,r.stderr)
finally:
    M.write_bytes(orig)
