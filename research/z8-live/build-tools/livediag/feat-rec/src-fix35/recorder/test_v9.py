#!/usr/bin/env python3
"""v9b actual linked pool layout/reuse + real LL/SC address checks (Unicorn, no QEMU).
Native allocator and open service outcomes are explicit stubs, not hardware evidence.
"""
from pathlib import Path
import re
support=Path(__file__).with_name('test_review9.py')
exec(compile(support.read_text().split('rows=[]')[0],str(support),'exec'))
# Move actual w5 state into the cacheable address window. Other fixture objects
# remain synthetic host RAM; never interpret Unicorn as a memory-type test.
ENC=RT+off['rt_enc']
fields=['arena','region','slot','reserved_diag_n','scratch','slot_bytes']
p=link/'v9-layout.c'
p.write_text('#include <stddef.h>\n#include "codec/live_w5enc.c"\nconst unsigned layout9[]={sizeof(struct live_w5),'+','.join('offsetof(struct live_w5,'+f+')' for f in fields)+'};\n')
subprocess.run(['clang','--target=aarch64-none-elf','-ffreestanding','-mgeneral-regs-only','-I',str(build),'-I',str(build/'codec/freestanding'),'-I',str(build/'ui/core/include'),'-c',str(p),'-o',str(link/'v9-layout.o')],check=True)
e=ELFFile(io.BytesIO((link/'v9-layout.o').read_bytes()));s=next(x for x in e.get_section_by_name('.symtab').iter_symbols() if x.name=='layout9');sec=e.get_section(s['st_shndx']);vals=struct.unpack('<7I',sec.data()[s['st_value']:s['st_value']+28]);size=vals[0];of=dict(zip(fields,vals[1:]))
exclusive={a:i for a,i in ins.items() if re.match(r'^(ldaxr|ldxr|ldaxp|ldxp|stlxr|stxr|stlxp|stxp|cas|swp|ldadd|ldset|ldclr|ldeor)',i.mnemonic)}
class InitMachine(Machine):
 def __init__(self,fail='',open_rc=0,unaligned=0):
  self.force_alive=None;self.fail=fail;self.open_rc=open_rc;self.unaligned=unaligned;self.allocs=[];self.frees=[];self.pool_n=0;self.opens=0;self.exclusives=[]
  super().__init__();self.u.mem_map(0xa03f31000,4096);self.u.mem_map(0xa02e98000,4096);self.u.mem_map(0xa02d40000,0x10000);self.w64(ENC+40,0)
 def hook(self,u,a,n,_):
  if a==sy['alive'] and self.force_alive is not None:self.ret(self.force_alive);return
  i=exclusive.get(a)
  if i:
   rn=re.search(r'\[(x\d+)\]',i.op_str)[1];addr=u.reg_read(xr[int(rn[1:])]);assert not 0x800000000<=addr<0xa00000000,(hex(a),hex(addr));self.exclusives.append((hex(a),i.mnemonic,hex(addr)))
  if a==sy['live_ui_engine_get']:self.ret(RT);return
  if a==0x40d2c408:
   self.allocs.append(u.reg_read(xr[0]));self.ret(0 if self.fail=='heap' else 0x800000000 if self.fail=='alias' else HW+self.unaligned);return
  if a==0x40d2c448 or a==0x4104d600:self.frees.append(u.reg_read(xr[0]));self.ret();return
  if a==0x4104d2dc:
   self.pool_n+=1;k=self.pool_n;need=u.reg_read(xr[2]);self.allocs.append(need)
   if self.fail==f'pool{k}':self.ret();return
   desc=0x225000+k*0x100;row=desc+32
   self.w64(desc+8,0 if self.fail==f'row{k}' else row)
   self.w64(row,0x800000000+k*0x10000000+(3 if self.fail=='unaligned_pool' else 0))
   self.w64(row+8,need-1 if self.fail==f'short{k}' else need)
   if self.fail=='bad_dma':self.w64(row,0x700000000)
   self.ret(desc);return
  if a==sy['open_stream']:self.opens+=1;self.ret(self.open_rc & 0xffffffff);return
  if a in (sy['memset'],sy['memcpy']):
   dst,src,count=[u.reg_read(xr[j]) for j in range(3)]
   u.mem_write(dst,bytes([src&255])*count if a==sy['memset'] else bytes(u.mem_read(src,count)));self.ret(dst);return
  super().hook(u,a,n,_)
rows=[]
for fail in ('pool1','pool2','row1','row2','short1','short2','bad_dma'):
 m=InitMachine(fail);rc=m.run('live_encoder_init',[ENC,0x220100,0x220200,1280,720]);assert rc&0xffffffff==0xffffffff
 assert not m.get(ENC+40,8) and not m.opens and not m.frees
 rows.append(dict(case=fail,allocs=m.allocs,rc=-1))
for rc in (0,-30,-3,-11):
 m=InitMachine(open_rc=rc);result=m.run('live_encoder_init',[ENC,0x220100,0x220200,1280,720])
 w=m.get(ENC+40,8);assert w==HW and m.get(w+off['rmw'],8)==RT
 assert not m.frees and m.opens==1 and m.get(w)==(0x57354c31 if rc in (0,-30) else 0)
 header=(size+4095)&~4095
 assert size==1057744 and header==1060864
 assert m.get(w+of['region'],8)==HW+header
 slot=m.get(w+of['slot_bytes']);assert slot==1658880
 region_sizes=[0x1400000,0x80000,0x500000,0x200000,0x200000,0x400000,0x200000,0x200000,0x200000,0x200000,0x80000]
 assert m.allocs==[16781312,header+sum(region_sizes)+4*slot+4096]
 for k in range(4):assert m.get(w+of['slot']+k*8,8)==HW+header+sum(region_sizes)+k*slot
 before=list(m.allocs);m.force_alive=1;m.w32(DIAG,7)
 retry=m.run('live_encoder_init',[ENC,0x220100,0x220200,1280,720])
 assert m.allocs==before and not m.frees and m.opens==1 and m.get(DIAG)==7
 assert retry&0xffffffff==(0 if rc in (0,-30) else 0xffffffff)
 rows.append(dict(case='init_and_reuse',open_rc=rc,allocs=m.allocs,published=hex(w)))
# Execute real diag CAS, suppress only the fixture's old w5_diag service stub.
m=InitMachine();m.w32(HW,0x57354c31)
old=sy['w5_diag']
orig_hook=Machine.hook
Machine.hook=lambda self,u,a,n,x: None if a==old else orig_hook(self,u,a,n,x)
m.run('w5_diag',[HW,1,0,1]);assert m.get(DIAG)==1
for entry,args in [('w5_close',[HW]),('live_encoder_preempt_lamp',[ENC,0,STATE,REGS])]:
 m.w64(ENC+40,HW);m.close_only=True;m.run(entry,args)
# Reset healthy stock state and exercise the armed close's inlined lock CAS too.
m.force_alive=1;m.clear_after=0;m.w32(HW+off['frames'],1)
m.w32(WSTATE,4);m.w32(INST,1);m.w32(INST+12,3);m.w64(VDI,1)
m.w32(STATE+off['diag.packed'],0)
m.run('live_encoder_preempt_lamp',[ENC,0,STATE,REGS])
stages=[p[0] for p in m.points]
assert all(v in stages for v in (15,17,19,48,49,54)),stages
# Each of the three actual codec CAS sites must have run on the intended field.
for name,field in [('w5_diag',DIAG),('w5_close',LOCK),('live_encoder_preempt_lamp',LOCK)]:
 sym=next(x for x in elf.get_section_by_name('.symtab').iter_symbols() if x.name==name)
 accesses=[x for x in m.exclusives if sym['st_value']<=int(x[0],16)<sym['st_value']+sym['st_size'] and x[1]=='ldaxr']
 assert any(int(x[2],16)==field for x in accesses),(name,accesses)
# Force contention once in both close implementations: execute their retry CAS sites.
for entry,args in [('w5_close',[HW]),('live_encoder_preempt_lamp',[ENC,0,STATE,REGS])]:
 n=InitMachine();n.close_only=True;n.force_alive=int(entry!='w5_close');n.clear_after=0
 n.w64(ENC+40,HW);n.w32(LOCK,1)
 n.peer=lambda machine: machine.w32(LOCK,0) if machine.time>=1000 else None
 n.run(entry,args);m.exclusives.extend(n.exclusives)
codec_names=('w5_diag','w5_close','live_encoder_preempt_lamp')
expected=set()
for sym in elf.get_section_by_name('.symtab').iter_symbols():
 if sym.name in codec_names:
  expected.update(a for a in exclusive if sym['st_value']<=a<sym['st_value']+sym['st_size'])
assert expected <= {int(x[0],16) for x in m.exclusives},expected-{int(x[0],16) for x in m.exclusives}
rows.append(dict(case='real_exclusive_addresses',accesses=m.exclusives))
report=dict(status='PASS',struct_bytes=size,heap_request=0,rows=rows,hardware_tested=False)
(link/'v9b-tests.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
