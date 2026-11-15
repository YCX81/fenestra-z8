#!/usr/bin/env python3
from pathlib import Path
import struct,json
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X30,UC_ARM64_REG_SP
D=Path(__file__).resolve().parent
CODE=0x40100000;E=0xa11000000;W=0x89f5ff000;INST=0xa02d41848;STACK=0xa13000000;ARENA=0x8a2e9b000;R=0xa12000000;STOP=0x40200000
results=[]
for codec in [0,1]:
 for name,wrong,expected in [('matching codec',None,0),('wrong codec',('type',1 if codec==0 else 3),1),('reused instance',('binding',INST+16),1),('rebound arena',('arena',ARENA+4096),1)]:
  stem=f'release-codec{codec}';syms={s[2]:int(s[0],16)for l in (D/f'{stem}.nm').read_text().splitlines() if len(s:=l.split())==3}
  u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
  for addr,n in [(CODE,0x10000),(STOP,4096),(E,4096),(W,0x103000),(STACK,0x10000),(R,0x10000),(0xa02d30000,0x20000),(0xa02e98000,4096),(0x40c33000,0x2000),(0x402f3000,4096),(0x401c6000,4096)]:u.mem_map(addr,n)
  u.mem_write(CODE,(D/f'{stem}.bin').read_bytes())
  def p(k,a,v):u.mem_write(a,struct.pack('<'+k,v))
  def q(k,a):return struct.unpack('<'+k,u.mem_read(a,struct.calcsize(k)))[0]
  for k,a,v in [('Q',E+40,W),('I',W,0x57354c31),('I',W+28,1),('Q',W+48,ARENA),('Q',W+1053592,R),('Q',W+1057016,INST),('Q',0xa02d3d980,0x16010000),('Q',0xa02d3d950,ARENA),('I',0xa02d30118,1),('I',0xa02d3274c,4),('Q',0xa02d32758,INST),('I',INST,1),('I',INST+12,1 if codec else 3)]:p(k,a,v)
  u.mem_write(0xa02e98480,b'\xff\xff\xff\xff\0')
  u.mem_write(0xa02d3d9d8,struct.pack('<Q',ARENA+0x200000)) # partition, not arena base
  if wrong:
   kind,val=wrong;p('I' if kind=='type' else 'Q',{'type':INST+12,'binding':W+1057016,'arena':0xa02d3d950}[kind],val)
  calls=[]
  def hook(uc,address,size,user):
   if address not in [0x40c34078,0x40c33fb0,0x40c340e8,0x402f3084,0x401c6cb4]:return
   if address==0x40c34078:calls.append('STOP')
   elif address==0x40c33fb0:
    calls.append('TRIGGER')
    if calls.count('TRIGGER')==1:p('I',0xa02d38b98+8,1);p('I',0xa02d38b98+0x60,1)
    else:p('I',0xa02d3274c,5)
   elif address==0x40c340e8:calls.append('FINISH');p('I',0xa02d3274c,0);p('I',INST,0)
   elif address==0x402f3084:calls.append('EXIT');p('Q',0xa02d3d980,0);p('I',0xa02d30118,0);p('B',0xa02e98484,255)
   elif address==0x401c6cb4:
    dst=uc.reg_read(UC_ARM64_REG_X0);src=uc.reg_read(UC_ARM64_REG_X1);n=uc.reg_read(UC_ARM64_REG_X2);uc.mem_write(dst,bytes(uc.mem_read(src,n)))
   uc.reg_write(UC_ARM64_REG_X0,0);uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_X30))
  from unicorn.arm64_const import UC_ARM64_REG_PC
  u.hook_add(UC_HOOK_CODE,hook)
  u.reg_write(UC_ARM64_REG_X0,E);u.reg_write(UC_ARM64_REG_X30,STOP);u.reg_write(UC_ARM64_REG_SP,STACK+0xfff0)
  u.emu_start(syms['live_encoder_release'],STOP,count=20000)
  assert u.reg_read(UC_ARM64_REG_PC)==STOP,(codec,name,'did not return')
  failed=q('I',W+1053564);stage=q('I',W+1053552)>>24
  assert stage==expected and failed==expected,(codec,name,stage,failed)
  assert calls==([] if expected else ['STOP','TRIGGER','TRIGGER','FINISH','EXIT']),(codec,name,calls)
  assert q('I',W+1053540)==1 and q('I',R+18988)==0 and q('I',W+32)==0
  results.append({'codec':codec,'case':name,'close_failed':failed,'failure_stage':stage,'native_order':calls,'passed':True})
(D/'close-arm-tests.json').write_text(json.dumps({'source':'actual ARM compiled live_encoder_release/w5_close; native call returns and resulting status writes modeled by harness','tests':results,'count':len(results),'physical_hardware_verified':False},indent=2)+'\n')
print(f'actual ARM close: {len(results)} cases passed, codec-specific guard and full native call order')
