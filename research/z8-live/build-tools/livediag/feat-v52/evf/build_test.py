from pathlib import Path
import ast,subprocess,json,hashlib
D=Path('/path/to/user/fxfix/v52-evf');S=D/'work';B=Path('/path/to/user/v37build/v51d');O=D/'pb';O.mkdir(exist_ok=True)
names=['hooks.S','runtime.c','preview.c','preview_writer.c','sync.c','native_sync.c','ui.c','native_ui.c','native_profile.c','settings.c']
flags=['--target=aarch64-none-elf','-Os','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-mgeneral-regs-only','-mstrict-align','-ffunction-sections','-fdata-sections','-Wall','-Wextra','-Werror']
objs=[]
for n in names:
 p=O/(n+'.o');subprocess.run(['clang-21',*flags,'-Oz','-I',str(S/'pb-fix37/core-include'),'-I',str(S/'pb-fix37'),'-c',str(S/'pb-fix37'/n),'-o',str(p)],check=True);objs.append(str(p))
subprocess.run(['ld.lld','-T',str(B/'pb/payload.ld'),*objs,'-o',str(O/'payload.elf')],check=True);subprocess.run(['llvm-objcopy','-O','binary',str(O/'payload.elf'),str(O/'payload.bin')],check=True)
sym={q[2]:int(q[0],16) for l in subprocess.check_output(['llvm-nm','-n',str(O/'payload.elf')],text=True).splitlines() if len(q:=l.split())==3}
(O/'symbols.json').write_text(json.dumps(sym,indent=2)+'\n')
# Exact owner flags, only the changed Live UI unit; other final integration units belong to root.
p=D/'live_ui.c.o';subprocess.run(['clang-21',*flags,'-DLA_WIDTH=1024','-DLA_HEIGHT=680','-DLA_RING_CAPACITY=96','-DLA_SAMPLE_INTERVAL_US=25000','-DLIVE_BITRATE=8000000','-DLIVE_IDR_US=500000','-DLIVE_ENCODE_SPEED=9','-I',str(S/'src-fix37'),'-I',str(S/'src-fix37/ui/core/include'),'-I',str(S/'src-fix37/codec/freestanding'),'-c',str(S/'src-fix37/ui/live_ui.c'),'-o',str(p)],check=True)
# Sum allocated section bytes; lets root budget without touching its combined link.
def size(p):
 out=subprocess.check_output(['llvm-size',str(p)],text=True).splitlines()[-1].split();return dict(text=int(out[0]),data=int(out[1]),bss=int(out[2]))
old=B/'live/ui_live_ui.c.o'
if not old.exists():old=next((B/'live').rglob('*live_ui.c.o'))
r=dict(all_pb_objects=10,pb_payload_bytes=(O/'payload.bin').stat().st_size,pb_baseline_bytes=(B/'pb/payload.bin').stat().st_size,live_ui_before=size(old),live_ui_after=size(p),warnings_as_errors=True)
(D/'build-tests.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r))
