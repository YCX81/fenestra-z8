"""Cheap checks against actual driver functions; does not import/run driver main."""
import ast,json,struct
from pathlib import Path
from types import SimpleNamespace
source=Path(__file__).with_name('run_live.py').read_text();tree=ast.parse(source)
functions=[node for node in tree.body if isinstance(node,ast.FunctionDef) and node.name in ('menu_state','adaptive_contact','direction','native_text_ids')]
ns={'LIVE_CONTACTS':[],'TAB_ENTERED':False,'QUALITY_SELECTED':False,'NATIVE_IDS':{},'struct':struct,'probe':SimpleNamespace(
 navigation_contact=lambda key:(key,'expected-'+key),key_body=lambda:'release',expected_keys=lambda:'released')}
exec(compile(ast.Module(body=functions,type_ignores=[]),'driver-functions','exec'),ns)
# Native current-page stack is top of vector; row is selected+tail.
mem={}
def put(address,value,size):
 for i,b in enumerate(value.to_bytes(size,'little')):mem[address+i]=b
g=SimpleNamespace(read_bytes=lambda address,size:bytes(mem.get(address+i,0) for i in range(size)))
base=0xa10000000;stack=base;data=base+0x100;page=base+0x200;desc=base+0x300;rows=base+0x400
put(0xa032b5160,stack,8);put(stack,8,4);put(stack+8,2,4);put(stack+16,data,8);put(data+8,page,8)
put(page,0x70000002,4);put(page+50,1,2);put(page+88,5,2);put(page+80,desc,8)
put(desc+16,rows,8);put(desc+32,8,2);put(rows+6*44+24,0x7fb2,4);put(rows+6*44,3177,4)
m=ns['menu_state'](g);assert m['page_id']==0x7000 and m['selected']==6 and m['row_id']==0x7fb2
# Original row tables are ROM; RAM-only validation had omitted their metadata.
romrows=0x43dcf790
put(desc+16,romrows,8);put(romrows+6*44+24,0x7fb2,4)
assert ns['menu_state'](g)['row_id']==0x7fb2
observed=[]
def step(kind,page,row=0,requested=0):
 item={'label':kind};ns['LIVE_CONTACTS']=[item]
 row_ids={663:[0x7ff0]+list(range(1,46)),0x7000:[0x7ff1,0x7ff2,0x7ff4,0x7ff5,0x7ff6,0x7ff7,0x7fb2,0x7ff3],0x705e:[0x7fb0,0x7fb1]}.get(page,[row])
 selected=row_ids.index(row) if row in row_ids else 0
 info={'menu':{'page_id':page,'row_id':row,'count':len(row_ids),'selected':selected,
               'rows':[{'index':i,'row_id':r} for i,r in enumerate(row_ids)]},'runtime':{'requested':requested}}
 ns['adaptive_contact'](None,'step-010-'+kind,info);observed.append(info['adaptive_key']);return info['adaptive_key']
# Legacy page fallback plus observed native tab-selector page1/index4.
for page_id in [638,639,640,641]:assert step('tab-seek-00',page_id)=='down'
for selected,expected in [(0,'up'),(3,'down'),(4,'right')]:
 item={'label':'tab-seek-00'};ns['LIVE_CONTACTS']=[item]
 info={'menu':{'page_id':1,'selected':selected,'count':7}}
 ns['adaptive_contact'](None,'step-010-tab-seek-00',info);assert info['adaptive_key']==expected,info
ns['TAB_ENTERED']=False
assert step('tab-seek-00',663)=='right'
assert step('tab-seek-00',663)=='release'
assert step('live-seek-00',663,0x7ff0)=='ok'
assert step('live-seek-00',0x7000,0x7ff1)=='up'
assert step('live-seek-00',0x7000,0x7ff3)=='up'
assert step('live-seek-00',0x7000,0x7fb2)=='ok'
assert step('live-seek-00',0x705e,0x7fb0)=='ok'
assert step('live-seek-00',0x7000,0x7fb2,1)=='release'
assert step('live-seek-00',12,0)=='release'

# Resolve pinned English labels through a synthetic original table layout.
put(0xa0281cd5c,65535,4);poolroot=base+0x10000;offsetroot=base+0x11000;array=base+0x12000;pool=0x4329f000
put(0xa02f2ac38,poolroot,8);put(0xa02f2ac48,offsetroot,8);put(poolroot+8,pool,8);put(offsetroot+8,array,8)
labels={1800:(0x432b92f0,'Image quality settings'),1801:(0x432a0258,'Image quality'),762:(0x432a5f30,'JPEG fine'),1105:(0x432a7fac,'PHOTO SHOOTING MENU')}
for index,(address,text) in labels.items():
 put(array+index*4,address-pool,4)
 for i,b in enumerate(text.encode('utf-16le')):mem[address+i]=b
ids=ns['native_text_ids'](g);assert ids==dict(quality_settings=1800,quality=1801,jpeg_fine=762,photo_title=1105),ids
# Normal quality path; HSFC row is not selected. JPEG fine triggers actual OK.
quality_keys=[]
for page_id,title,selected,rows in [(25,1105,0,[1,2,3,4,5,6,7,8,1800]),(25,1105,8,[1,2,3,4,5,6,7,8,1800]),
                            (44,2417,0,[760,2418]),(45,2417,10,list(range(15))),(45,2417,8,list(range(15)))]:
 item={'label':'quality-seek-00'};ns['LIVE_CONTACTS']=[item]
 info={'menu':{'page_id':page_id,'title_id':title,'selected':selected,'count':len(rows),
               'rows':[{'index':i,'text_id':t} for i,t in enumerate(rows)]}}
 ns['adaptive_contact'](g,'step-010-quality-seek-00',info);quality_keys.append(info['adaptive_key'])
assert quality_keys==['up','ok','ok','up','ok'],quality_keys
assert ns['QUALITY_SELECTED']
assert ns['direction']({'selected':44,'count':46},0)=='down'
assert ns['direction']({'selected':23,'count':46},0) in ('down','up')

result=dict(pass_=True,scope='Synthetic decoder and decision-unit checks only; see run screenshots for runtime results',quality_keys=quality_keys,native_text_ids=ids,decoded_menu=m,adaptive_keys=observed,guest_ram_written=False,
 previous_route_evidence='/path/to/user/.local/share/nikon-z8-qemu/fenestra-z8/mem-c36c702e-pair01/step-014-nav-6-ok.png',
 previous_route_result='menu,left,right,up,up,ok reached Pixel shift merge, not Fenestra')
Path(__file__).with_name('navigation-test.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
