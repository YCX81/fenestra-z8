from pathlib import Path
import re,json,hashlib,difflib
P=Path(__file__).resolve().parent;rd=(P.parent/'baseline.c').read_text();mg=(P/'baseline.c').read_text();candidate=(P/'candidate.c').read_text()
def function(s,name):
 m=re.search(r'static\s+[^;{}]*\b'+name+r'\([^;{}]*\)\s*\{',s);assert m,name
 i=m.start();j=m.end();n=1
 while n:n+=(s[j]=='{')-(s[j]=='}');j+=1
 return s[i:j]
def normal(s):return s.replace('NikonZ8','NikonZ').replace('NikonZ9','NikonZ').replace('Z8Serial','ZSerial').replace('Z9Serial','ZSerial').replace('Z8_SERIAL','Z_SERIAL').replace('Z9_SERIAL','Z_SERIAL')
rows=[]
for name in ['serial_enable','serial11_config','pdma_ram_address']:
 a,b=map(normal,[function(rd,name),function(mg,name)]);assert a==b,name;rows.append(dict(function=name,equal_after_type_logprefix_only=True,sha256=hashlib.sha256(a.encode()).hexdigest()))
a=normal(function(rd,'serial_pio_update'));b=normal(function(mg,'serial_pio_update'));assert b.replace('    serial_clock_hold_update(s);\n','')==a
rows.append(dict(function='serial_pio_update',only_existing_merged_difference='serial_clock_hold_update(s); unchanged by port'))
a=function(rd,'dma_ram_range');b=function(mg,'dma_ram_range');assert a[a.index('    for (unsigned i = 0; i < ARRAY_SIZE(dsp_load_ram)'):]==b[b.index('    for (unsigned i = 0; i < ARRAY_SIZE(dsp_load_ram)'):]
rows.append(dict(function='dma_ram_range',common_main_guest_RAM_validation_identical=True,only_difference='earlier optional image bank range enumeration API; this peer targets original main RAM'))
peer=(P.parent/'peer.inc').read_text();assert peer in candidate
rx=(P.parents[2]/'audio/rx16-model/rx16.patch').read_text();added='\n'.join(x[1:] for x in rx.splitlines() if x.startswith('+') and not x.startswith('+++'))+'\n';code=added.split('    object_class_property_add_str(oc,"serial5-pcm16-input"',1)[0];assert code in candidate
assert 's->serial5_pcm_phase[(addr - 0x0e200100) / 0x100] = 0;' in mg
res=dict(pass_all=True,source_sha256=hashlib.sha256(candidate.encode()).hexdigest(),spi_function_bytes_identical=True,rx16_function_bytes_identical=True,spi_source_sha256=hashlib.sha256(peer.encode()).hexdigest(),rx16_code_sha256=hashlib.sha256(code.encode()).hexdigest(),dependency_checks=rows,original_phase_reset_retained=True,boundary='Static exact function/dependency comparison plus prior actualC/ARM tests. No whole merged core result is asserted.')
(P/'port-tests.json').write_text(json.dumps(res,indent=2)+'\n');print(json.dumps(res,indent=2))
