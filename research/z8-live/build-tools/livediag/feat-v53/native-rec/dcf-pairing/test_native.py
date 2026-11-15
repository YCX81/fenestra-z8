"""Exact v52G DCF call contracts. Filesystem, node locator, locks and signals are leaves.
Run in WSL with the QEMU venv. No camera, card image or production file is mutated.
"""
from pathlib import Path
import hashlib, struct, json, os
from unicorn import *
from unicorn.arm64_const import *
P=Path(__file__).resolve().parent
IMAGE=Path(os.environ.get('DCF_IMAGE','/path/to/user/fxfix/v52-root/v52g/q/main.main'))
B=0x40160000; data=IMAGE.read_bytes(); SHA=hashlib.sha256(data).hexdigest()
assert SHA=='ed1e8496fccde443412e527a252de5029a55c749053b9b046da42bfc454aa613'
OK=0x04320080
class M:
 def __init__(self):
  self.u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u=self.u
  u.mem_map(B,(len(data)+4095)&~4095);u.mem_write(B,data)
  u.mem_map(0x4e000000,0x400000);u.mem_map(0xa0481f000,0x4000)
  self.end=0x4e001000;self.sp=0x4e00f000;self.obj=0x4e002000;self.node=0x4e004000;self.storage=0x4e100000
  self.log=[];self.refs={};self.store_fail=False;self.unlink_fail=0;self.node_present=True;self.hooks={}
  self.w64(0xa04822af8,self.storage);self.w64(0xa04822b00,self.storage+0x80000)
  u.hook_add(UC_HOOK_CODE,self.code)
 def r32(self,a):return struct.unpack('<I',self.u.mem_read(a,4))[0]
 def r64(self,a):return struct.unpack('<Q',self.u.mem_read(a,8))[0]
 def w32(self,a,v):self.u.mem_write(a,struct.pack('<I',v&0xffffffff))
 def w64(self,a,v):self.u.mem_write(a,struct.pack('<Q',v&0xffffffffffffffff))
 def w16(self,a,v):self.u.mem_write(a,struct.pack('<H',v))
 def r(self,n):return self.u.reg_read(UC_ARM64_REG_X0+n)
 def ret(self,v=0):self.u.reg_write(UC_ARM64_REG_X0,v&0xffffffffffffffff);self.u.reg_write(UC_ARM64_REG_PC,self.u.reg_read(UC_ARM64_REG_LR))
 def call(self,pc,*args):
  self.u.reg_write(UC_ARM64_REG_SP,self.sp);self.u.reg_write(UC_ARM64_REG_LR,self.end)
  for n,v in enumerate(args):self.u.reg_write(UC_ARM64_REG_X0+n,v)
  self.u.emu_start(pc,self.end,count=100000);assert self.u.reg_read(UC_ARM64_REG_PC)==self.end,hex(self.u.reg_read(UC_ARM64_REG_PC))
  return self.r(0)&0xffffffff
 def record(self,a,id,size=54321,mask=None,protect=0):
  self.u.mem_write(a,bytes(0x70));self.w32(a,id);self.w64(a+8,a+0x20);self.w64(a+0x18,size);self.w16(a+0x24,mask if mask is not None else 1<<(id&15));self.u.mem_write(a+0x10,bytes([4,protect]))
 def file(self,a,id,size=54321,mask=None,protect=0):
  self.u.mem_write(a,bytes(0x100));self.record(a+0x28,id,size,mask,protect);self.w32(a+0x70,1);self.refs[id]=a
 def code(self,u,pc,sz,_):
  if pc in self.hooks:return self.hooks[pc](self)
  if pc in (0x4085d348,0x40875834,0x408758b4,0x40865784,0x4085aab8,0x4085d2e0):self.ret()
  elif pc==0x4085d364:self.ret(7) # intentional arbitrary unlock result
  elif pc==0x4127a900:
   dest,src,n=self.r(0),self.r(1),self.r(2);u.mem_write(dest,bytes(u.mem_read(src,n)));self.ret(dest)
  elif pc==0x4127ab40:
   dest,v,n=self.r(0),self.r(1),self.r(2);u.mem_write(dest,bytes([v&255])*n);self.ret(dest)
  elif pc==0x40860550:
   rec=self.r(1);self.log.append(dict(call='store_commit_leaf',card_storage=hex(self.r(0)),record=hex(rec),failure=self.store_fail))
   if not self.store_fail:
    u.mem_write(self.node,bytes(u.mem_read(rec,0x48)));self.w64(self.node+8,self.node+0x20);self.w16(self.node+0x48,0)
   self.ret(OK+4 if self.store_fail else OK)
  elif pc in (0x408648a8,0x40864b1c):self.ret()
  elif pc==0x40875b3c:
   self.log.append(dict(call='notification',event=hex(self.r(0)),id=self.r32(self.r(1)),size=self.r64(self.r(1)+8)));self.ret(13)
  elif pc==0x408714e8:
   self.log.append(dict(call='index_lookup',tree=hex(self.r(0)),id=self.r(1)));self.ret(self.node if self.node_present else 0)
  elif pc==0x40871884:self.ret(0) # known-valid, not deleted index node
  elif pc==0x4085f59c:self.ret(0) # no fallback directory cache
  elif pc==0x408715c0:self.ret(self.node if self.node_present else 0)
  elif pc==0x4085c018:self.ret(self.r(0)+0xa0) # owned path pointer
  elif pc==0x402dada8:
   self.log.append(dict(call='unlink',file=hex(self.r(0)-0xa0)));self.ret(self.unlink_fail)
  elif pc==0x40863684:self.ret(OK+9)
  elif pc in (0x4085d470,0x4085d3b4):self.ret(0)
  elif pc in (0x4085a90c,0x4085d200):
   self.log.append(dict(call='file_lookup',id=self.r(0)));self.ret(self.refs.get(self.r(0),0))
  elif pc==0x40864f20:
   self.log.append(dict(call='update_group',removed_mask=self.r(2)));self.ret(OK)

results=[]
for card in (0,1):
 id=(card<<29)|(100<<19)|(9014<<5)|5
 for fail in (False,True):
  m=M();m.store_fail=fail;m.file(m.obj,id,10);m.w32(m.obj+0x70,2);m.record(m.node,id,10);m.w16(m.node+0x48,1<<5)
  rc=m.call(0x4085c4bc,m.obj,9876543210)
  assert rc==7 and m.r32(m.obj+0x70)==1 and m.r64(m.obj+0x40)==9876543210
  out=0x4e006000;flag=out+0x50;m.u.mem_write(out,b'\xa5'*0x60)
  read_rc=m.call(0x40865210,id,out,flag); registered=m.u.mem_read(flag,1)[0]
  assert read_rc==OK and registered==int(not fail)
  assert m.r64(out+0x18)==(10 if fail else 9876543210)
  assert m.u.mem_read(out+0x48,8)==b'\xa5'*8 and m.u.mem_read(flag+1,15)==b'\xa5'*15
  assert len([x for x in m.log if x['call']=='notification'])==int(not fail)
  results.append(dict(test='commit_then_independent_read',card=card,store_fail=fail,cached_state=1,commit_return=rc,read_rc=hex(read_rc),registered=registered,index_size=m.r64(out+0x18),calls=m.log))
 # Actual 71cf8 clears only the movie pending bit and writes actual size.
 m=M();m.record(m.obj,id,777);m.record(m.node,id,11);m.w16(m.node+0x48,1<<5)
 rc=m.call(0x40871cf8,m.storage+0x203280,m.obj,0x4e006000,0x4e006008,0x4e006010)
 assert rc==OK and m.r64(m.node+0x18)==777 and struct.unpack('<H',m.u.mem_read(m.node+0x48,2))[0]==0
 results.append(dict(test='native_commit_node_transition',card=card,rc=hex(rc),node_size=m.r64(m.node+0x18),pending=0))
 # No node does not become registered merely because cached object is state1.
 m=M();m.node_present=False;out=0x4e006000;flag=out+0x50
 rc=m.call(0x40865210,id,out,flag);assert rc==OK+5 and m.u.mem_read(flag,1)==b'\0'
 results.append(dict(test='missing_index_node',card=card,rc=hex(rc),registered=0))

for card in (0,1):
 stem=(card<<29)|(100<<19)|(9014<<5);mid=stem|5;jid=stem|0;nid=stem|2
 for all_group in (0,1):
  for protected in (0,1):
   m=M();mask=(1<<0)|(1<<2)|(1<<5)
   m.file(m.obj,mid,777,mask,protected);m.file(m.obj+0x200,jid,111,mask);m.file(m.obj+0x400,nid,222,mask)
   originals=[bytes(m.u.mem_read(m.obj+o+0x28,0x48)) for o in (0x200,0x400)]
   rc=m.call(0x4085ae90,m.obj,all_group)
   unlinks=[x['file'] for x in m.log if x['call']=='unlink']
   assert rc==(OK+11 if protected else OK),(hex(rc),m.log)
   if protected:assert not unlinks
   elif not all_group:assert unlinks==[hex(m.obj)],unlinks
   else:assert set(unlinks)=={hex(m.obj),hex(m.obj+0x200),hex(m.obj+0x400)},unlinks
   results.append(dict(test='native_delete_group',card=card,delete_linked_formats=all_group,protected=protected,rc=hex(rc),unlinks=unlinks,calls=m.log))

(P/'native-tests.json').write_text(json.dumps(dict(image_sha256=SHA,passed=True,cases=results,boundary='Actual G c4bc/647c4 + independent 65210/fa38/72e4c/72f1c + node commit71cf8/71e98 + ae90/ba30 and record helpers. Locks, storage locator, final store_commit (separate node-transition test), file locator, filesystem unlink and notification leaves stubbed; no actual filesystem/card commit, no worker/context proof.'),indent=2)+'\n')
print('PASS',len(results),'exact-ARM cases')
