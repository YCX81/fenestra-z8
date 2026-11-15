#!/usr/bin/env python3
"""Create a private V62 capture driver and fresh AArch64 ABI tables.

Does not launch QEMU, change firmware, or write camera media.
"""
import argparse, hashlib, json, re, shutil, struct, subprocess
from pathlib import Path
from elftools.elf.elffile import ELFFile

ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--source',type=Path,required=True)
ap.add_argument('--output',type=Path,required=True)
ap.add_argument('--reference',type=Path,default=Path('/path/to/user/fxfix/v53-audio/core-pcm-diag-g/driver'))
a=ap.parse_args()
assert not a.output.exists()
shutil.copytree(a.reference,a.output,symlinks=True)
sha=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest()
reports={}

def layout(name,typ,fields,includes,source):
    code=includes+'\n#include <stddef.h>\n'
    code+='const unsigned __attribute__((section(".verification_layout"))) verification_layout[]={sizeof('+typ+'),\n'
    code+=',\n'.join('offsetof('+typ+','+f+'),sizeof((('+typ+'*)0)->'+f+')' for f in fields)+'\n};\n'
    c=a.output/(name+'-v62-layout.c');obj=c.with_suffix('.o');c.write_text(code)
    cmd=['clang-21','--target=aarch64-none-elf','-Os','-ffreestanding','-mgeneral-regs-only','-mstrict-align',
         '-DLA_WIDTH=1024','-DLA_HEIGHT=680','-DLA_RING_CAPACITY=96','-DLA_SAMPLE_INTERVAL_US=25000',
         '-I'+str(source),'-I'+str(source/'ui/core/include'),'-I'+str(source/'codec/freestanding'),
         '-I'+str(source/'core-include'),'-c',str(c),'-o',str(obj)]
    subprocess.run(cmd,check=True)
    with obj.open('rb') as f:raw=ELFFile(f).get_section_by_name('.verification_layout').data()
    words=struct.unpack('<'+'I'*(len(raw)//4),raw);assert len(words)==1+2*len(fields)
    offsets=dict(zip(fields,words[1::2]));widths=dict(zip(fields,words[2::2]));offsets['struct_bytes']=words[0]
    (a.output/(name+'-offsets.json')).write_text(json.dumps(offsets,indent=2)+'\n')
    (a.output/(name+'-widths.json')).write_text(json.dumps(widths,indent=2)+'\n')
    reports[name]={'struct_bytes':words[0],'offsets':offsets,'widths':widths,'command':cmd,
                   'layout_source_sha256':sha(c),'layout_object_sha256':sha(obj)}

runtime_fields=list(json.loads((a.reference/'runtime-offsets.json').read_text()))
runtime_fields.remove('struct_bytes')
for f in ['shutdown','card_epoch','shutter.params','shutter.fire_params']:
    if f not in runtime_fields:runtime_fields.append(f)
# Only expose real scalar/current fields; payload structs remain in raw dump.
runtime_fields=[f for f in runtime_fields if f not in ['shutter.params','shutter.fire_params']]
layout('runtime','struct live_runtime',runtime_fields,'#include "runtime.h"',a.source/'src-fix37')
pb_fields=list(json.loads((a.reference/'pb-offsets.json').read_text()))
pb_fields.remove('struct_bytes')
for f in '''magic bytes started error current_id cache_valid lookup_count lookup_error frames playing
native_path directory consumed_rec nb_request nb_stage nb_photo nb_movie nb_starts nb_returns
nb_redirects nb_w nb_h nb_bit nb_fail nb_cleanups nb_from nb_to'''.split():
    if f not in pb_fields:pb_fields.append(f)
layout('pb','PbRuntime',pb_fields,'#include "runtime.h"',a.source/'pb-fix37')
ui_fields='magic engine save_error loaded generation mute_live shutter_seen shutter_suppressed icon_phase icon_save_error'.split()
ui_c=a.source/'src-fix37/ui/live_ui.c'
layout('ui','UiState',ui_fields,'#include "'+str(ui_c)+'"',a.source/'src-fix37')
reports['source_files']={str(p.relative_to(a.source)):sha(p) for p in [a.source/'src-fix37/runtime.h',a.source/'src-fix37/audio/live_shutter_z8.h',a.source/'pb-fix37/runtime.h',ui_c]}
(a.output/'v62-layout-provenance.json').write_text(json.dumps(reports,indent=2)+'\n')
print(json.dumps({k:v['struct_bytes'] for k,v in reports.items() if k!='source_files'}))
