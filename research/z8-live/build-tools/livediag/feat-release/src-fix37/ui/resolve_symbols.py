#!/usr/bin/env python3
"""Pin reused core and publish guarded hook/link inputs to the parent builder."""
import json, hashlib, struct
from pathlib import Path
HERE=Path(__file__).resolve().parent
HOME=Path.home()/'.local/share/nikon-z8-qemu/fenestra-z8/bundle'
BUILD=HOME/'bridge64-fccb179/tree/build/z8-core'
firmware=(HOME/'fix-20261104.main').read_bytes()
core=(BUILD/'core.bin').read_bytes()
assert len(core)==268776
assert hashlib.sha256(core).hexdigest()=='bc01ea90f042e036d2894879156c33f80873e8ad472295ebfe89dc9275c642a6'
assert firmware[0x40000:0x40000+len(core)]==core
symbols=json.loads((BUILD/'build.json').read_text())['symbols']
aliases={'live_old_menu_page':'fx_z8_menu_page','live_old_menu_text':'fx_z8_menu_text',
         'live_old_system_dispatch':'fx_z8_system_dispatch'}
used=['fx_native_menu_build','fx_native_menu_link','fx_native_display_attach_global',
      'fx_native_display_present','fx_native_display_detach','fx_native_display_is_current','memset','memcpy','fx_memset']
lines=[f'{name} = 0x{symbols[source]:x};' for name,source in aliases.items()]
lines += [f'{name} = 0x{symbols[name]:x};' for name in used]
(HERE/'existing_core.ld').write_text('\n'.join(lines)+'\n')
hooks=[]
for address,source,target in [(0x403db694,'fx_z8_menu_page','live_ui_menu_page'),
                              (0x4032e130,'fx_z8_menu_text','live_ui_menu_text'),
                              (0x40d60ec8,'fx_z8_system_dispatch','live_ui_system_dispatch')]:
    before=firmware[address-0x40160000:address-0x40160000+4]
    insn=struct.unpack('<I',before)[0]; imm=insn&0x3ffffff
    delta=(imm-(1<<26) if imm&(1<<25) else imm)*4
    assert insn>>26==5 and address+delta==symbols[source]
    hooks.append(dict(address=hex(address),expected=before.hex(),target=target))
(HERE/'integration.json').write_text(json.dumps(dict(
    core_sha256=hashlib.sha256(core).hexdigest(),core_start='0x401a0000',core_bytes=len(core),
    source='ui/live_ui.c',include='ui/core/include',linker='ui/existing_core.ld',hooks=hooks,
    state_root='stock root[2]+663*96 -> descriptor +2528 -> UiState.engine',
    live_page='0x705e',live_text=3177,menu_row_index=6),indent=2)+'\n')
print(json.dumps({'core_verified':True,'hooks':hooks}))
