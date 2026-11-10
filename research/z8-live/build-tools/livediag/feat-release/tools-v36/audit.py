"""Inventory actual object instructions and map to clang assembly source locations.
Requires pyelftools/capstone. No execution/emulation of target code.
"""
from pathlib import Path
import re,json,io
from elftools.elf.elffile import ELFFile
from capstone import Cs,CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN
ROOT=Path(__file__).resolve().parent.parent
OUT=ROOT/'audit-v36'
rmw=re.compile(r'^(?:ldaxr|ldxr|stlxr|stxr|ldaxp|ldxp|stlxp|stxp|cas|swp|ldadd|ldclr|ldeor|ldset|ldsmax|ldsmin|ldumax|ldumin)')
d=Cs(CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN)
rows=[]
for obj in sorted([*OUT.glob('src/*.o'),*OUT.glob('pb/*.o')]):
    elf=ELFFile(io.BytesIO(obj.read_bytes()));asm=obj.with_suffix('.s');mapping={};files={};func=None;loc=None
    if asm.exists():
        lines=asm.read_text().splitlines()
        for n,line in enumerate(lines):
            m=re.match(r'\s*\.file\s+(\d+)\s+"([^"]+)"\s+"([^"]+)"',line)
            if m: files[int(m[1])]=str((ROOT/Path(m[2])/m[3]).resolve().relative_to(ROOT)) if str((ROOT/Path(m[2])/m[3]).resolve()).startswith(str(ROOT)+'/') else str(Path(m[2])/m[3])
            m=re.match(r'\s*\.type\s+([^,]+),@function',line)
            if m:func=m[1];mapping[func]=[]
            m=re.match(r'\s*\.loc\s+(\d+)\s+(\d+)\s+(\d+)',line)
            if m:loc=(files[int(m[1])],int(m[2]),int(m[3]))
            instruction=line.strip().split('//')[0].strip()
            if rmw.match(instruction):
                mapping[func].append((instruction,loc,n+1))
    total_rmw=sum(1 for sec in elf.iter_sections() if sec['sh_flags']&4 for i in d.disasm(sec.data(),0) if rmw.match(i.mnemonic))
    before=len(rows)
    assert not [s.name for s in elf.get_section_by_name('.symtab').iter_symbols() if s['st_shndx']=='SHN_UNDEF' and re.match(r'__(atomic|sync|aarch64_)',s.name)]
    for sym in elf.get_section_by_name('.symtab').iter_symbols():
        if sym['st_info']['type']!='STT_FUNC' or not isinstance(sym['st_shndx'],int):continue
        sec=elf.get_section(sym['st_shndx']);code=sec.data()[sym['st_value']:sym['st_value']+sym['st_size']]
        ins=list(d.disasm(code,0));ex=[(k,i) for k,i in enumerate(ins) if rmw.match(i.mnemonic)]
        match=mapping.get(sym.name,[])
        assert len(ex)==len(match),(obj,sym.name,len(ex),len(match))
        for (k,i),(a,loc,lineno) in zip(ex,match):
            normalize=lambda s:re.sub(r'\s+','',s).replace('#0x','#')
            assert normalize(i.mnemonic+' '+i.op_str)==normalize(a),(obj,sym.name,i.mnemonic,i.op_str,a)
            file,line,col=loc
            source=(ROOT/file).read_text().splitlines()[line-1] if line else ''
            rows.append(dict(object=str(obj.relative_to(ROOT)),function=sym.name,offset=hex(i.address),instruction=i.mnemonic+' '+i.op_str,base_register=re.search(r'\[(\w+)',i.op_str)[1],source_file=file,source_line=line,column=col,source=source.strip(),assembly=str(asm.relative_to(ROOT))+':'+str(lineno),context=[f'{j.address:#x}: {j.mnemonic} {j.op_str}' for j in ins[max(0,k-14):k+1]]))
    assert len(rows)-before==total_rmw,(obj,'unmapped executable RMW')
(OUT/'exclusive-instructions.json').write_text(json.dumps(rows,indent=2)+'\n')
print('Object/assembly RMW matching:',len(rows),'instructions')
for key in sorted(set((r['source_file'],r['source_line'],r['source']) for r in rows)):
 print(*key)

def origin(file,line):
    src=(ROOT/file).read_text().splitlines()
    text=' '.join(src[line-1:line+2])
    if file.endswith('fx_registry.h'):
        if line==42:return 'core root + 2528 (events[46]); root from [root+663*96+80], cached RAM range checked', 'core 常驻 RAM / 0xa 窗口'
        if line==55:return 'FxRegistry.live_ui (+8); registry allocated by 0x40d2c408(size,0)', '缓存堆'
        if line==61:return 'FxRegistry.menu_lock (+24); registry allocated by 0x40d2c408(size,0)', '缓存堆'
    if file.endswith('codec/live_w5enc.c'):
        return 'w (pool) -> ordinary LDR at w+1053592 -> runtime +18908; x23=w+0x101000, LDR [x23,#920]; x8=rmw+0x49dc', '缓存堆；仅指针存于 pool'
    if file=='pb-fix36/runtime.c' and line==243:return 'slot_create -> &FxRegistry.playback (+16); registry allocated by 0x40d2c408(size,0)', '缓存堆'
    match=re.search(r'(?:__atomic_(?:add_fetch|fetch_add|compare_exchange_n|exchange_n)|atomic_(?:fetch_\w+|exchange_\w+|compare_exchange_\w+))\s*\(\s*&([\w.>\-]+)',text)
    assert match,(file,line,text)
    field=match[1]
    if file=='pb-fix36/runtime.c':return field+'; PbRuntime from registry.playback, pb_boot -> 0x40d2c408(sizeof,0)', '缓存堆'
    if file=='pb-fix36/native_ui.c':return field+'; d=&PbRuntime.display.display; pb_boot -> 0x40d2c408(sizeof,0)', '缓存堆'
    if '/ui/' in file:return field+'; UiState / embedded display; install -> 0x40d2c408(sizeof,0)', '缓存堆'
    if '/capture/live_capture.c' in file or '/capture/live_stream.c' in file:return field+'; c=&runtime.capture (+8); live_runtime_boot -> 0x40d2c408(sizeof,0)', '缓存堆'
    if file.endswith('capture/live_stream.h'):return field+'; s=&runtime.stream (+2880); live_runtime_boot -> 0x40d2c408(sizeof,0)', '缓存堆'
    if file=='src-fix36/runtime.c':return field+'; state()/worker argument -> runtime; live_runtime_boot -> 0x40d2c408(sizeof,0)', '缓存堆'
    raise AssertionError((file,line))
for r in rows:
    r['base_origin'],r['memory']=origin(r['source_file'],r['source_line'])
(OUT/'exclusive-instructions.json').write_text(json.dumps(rows,indent=2,ensure_ascii=False)+'\n')
md=['# v36 对象逐条独占访问','',f'共 {len(rows)} 条。地址为对象内函数相对偏移，不是假定的最终 MAIN 地址。',
'生产对象不带调试；另用相同参数生成 `-g -S`，逐函数核对全部独占指令的数量、顺序、操作码和寄存器完全一致。基址来源结合源代码分配与调用链人工分类；不是 MMU 属性实测。JSON 附每条指令前 14 条实际对象反汇编，完整 `.s` 含源位置与 DEBUG_VALUE。',
'','|对象 / 函数+偏移|指令|源位置|基址来源|内存|','|---|---|---|---|---|']
for r in rows:
    md.append('|'+ '|'.join([r['object']+' / '+r['function']+'+'+r['offset'],'`'+r['instruction']+'`',r['source_file']+':'+str(r['source_line']),r['base_register']+' = '+r['base_origin'],r['memory']])+'|')
(OUT/'exclusive-audit.md').write_text('\n'.join(md)+'\n')
# Entire-tree source inventory: includes dead inline helpers, no silent omission.
pattern=re.compile(r'\b(?:__atomic_(?:\w*fetch\w*|exchange\w*|compare_exchange\w*|test_and_set|clear)|__sync_\w+|atomic_(?:fetch_\w+|exchange\w*|compare_exchange\w*|flag_\w+))\s*\(')
inventory=[]
for tree in ('src-fix36','pb-fix36'):
 for p in sorted((ROOT/tree).rglob('*')):
  if not p.is_file() or p.suffix not in ('.c','.h','.S') or p.name.startswith('._'):continue
  for n,s in enumerate(p.read_text().splitlines(),1):
   if pattern.search(s):
    file=str(p.relative_to(ROOT));emitted=[r for r in rows if r['source_file']==file and r['source_line']==n]
    if 'fenestra_runtime_accounts.h' in file:detail=('FxRuntimeAccounts.state.flags; caller-owned worker/service record; no callers in these payloads','未实例化 API；调用方内存，不可由本树确认')
    elif 'fenestra_native_ui.h' in file:detail=('tag; caller-supplied retained-account tag; no callers in these payloads','未实例化 API；调用方内存，不可由本树确认')
    else:detail=origin(file,n)
    inventory.append(dict(file=file,line=n,source=s.strip(),base_origin=detail[0],memory=detail[1],instructions=len(emitted)))
(OUT/'source-rmw.json').write_text(json.dumps(inventory,indent=2,ensure_ascii=False)+'\n')
md=['# 全树 RMW 源码清单','','|位置|操作|对象/来源|内存|对象指令数|','|---|---|---|---|---|']
for r in inventory:md.append('|'+ '|'.join([r['file']+':'+str(r['line']),r['source'].replace('|','\\|'),r['base_origin'],r['memory'],str(r['instructions'])])+'|')
(OUT/'source-rmw.md').write_text('\n'.join(md)+'\n')
