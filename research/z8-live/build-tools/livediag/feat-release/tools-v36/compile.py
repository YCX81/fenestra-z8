"""Compile every shipping translation unit; emit matching assembly for RMW audit."""
from pathlib import Path
import subprocess,json,hashlib
ROOT=Path(__file__).resolve().parent.parent
OUT=ROOT/'audit-v36';OUT.mkdir(exist_ok=True)
LIVE='runtime.c hooks.S capture/live_capture.c capture/native_capture.c ui/live_ui.c codec/live_w5enc.c codec/live_h264_idr.c codec/live_mov.c codec/live_jpeg.c codec/live_transaction.c codec/native_files.c codec/au_ring.c capture/live_stream.c codec/preview_writer.c audio/live_audio.c audio/live_audio_mov.c audio/live_audio_native.c audio/live_audio_z8.c'.split()
PB='hooks.S runtime.c preview.c preview_writer.c sync.c native_sync.c ui.c native_ui.c native_profile.c settings.c'.split()
FLAGS='--target=aarch64-none-elf -Os -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -mgeneral-regs-only -mstrict-align -Wall -Wextra -Werror'.split()
DEFS='-DLA_WIDTH=1024 -DLA_HEIGHT=680 -DLA_RING_CAPACITY=96 -DLA_SAMPLE_INTERVAL_US=25000 -DLIVE_BITRATE=8000000 -DLIVE_IDR_US=500000 -DLIVE_ENCODE_SPEED=9'.split()
commands=[];results=[]
for group,names in [('src',LIVE),('pb',PB)]:
    src=ROOT/('src-fix36' if group=='src' else 'pb-fix36');out=OUT/group;out.mkdir(exist_ok=True)
    includes=[src,src/'ui/core/include',src/'codec/freestanding'] if group=='src' else [src,src/'core-include',ROOT/'src-fix36/codec/freestanding']
    for name in names:
        base=['clang',*FLAGS,*(DEFS if group=='src' else []),*(x for p in includes for x in ['-I',str(p)])]
        if group=='pb' and name in ('ui.c','native_ui.c'): base+=['-Oz']
        obj=out/(name.replace('/','_')+'.o');asm=obj.with_suffix('.s')
        for opts,dest in [(['-c'],obj),(['-g','-S'],asm)] if name.endswith('.c') else [(['-c'],obj)]:
            cmd=base+opts+[str(src/name),'-o',str(dest)];commands.append(cmd)
            r=subprocess.run(cmd,text=True,capture_output=True)
            results.append({'source':str((src/name).relative_to(ROOT)),'output':str(dest.relative_to(ROOT)),'rc':r.returncode,'stdout':r.stdout,'stderr':r.stderr})
            if r.returncode or r.stderr: print(r.stderr);raise SystemExit('compile failed or emitted diagnostics')
        print(group,name,'OK')
(OUT/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
(OUT/'compile-results.json').write_text(json.dumps({'clang':subprocess.check_output(['clang','--version'],text=True),'units':len(LIVE)+len(PB),'results':results},indent=2)+'\n')
