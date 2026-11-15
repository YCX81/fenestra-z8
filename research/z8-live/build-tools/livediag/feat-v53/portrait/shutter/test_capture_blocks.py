#!/usr/bin/env python3
"""Actual G native 109/106 -> 709 descriptor and 111 callback size propagation.
Lower DMA queue remains an observation boundary: no physical capture timing.
"""
from pathlib import Path
import json,struct,hashlib
P=Path(__file__).resolve().parent;A=P.parents[1]/'audio';FW=P.parent/'v52g.main'
s=(A/'test_native_output.py').read_text();s=s[:s.index('res=[scenario(')]
s=s.replace("P=Path(__file__).resolve().parent;FW=P.parents[1]/'feat-v50/integration-writer/v51d/q/main.main';D=FW.read_bytes();B=0x40160000",f"P=Path({str(P)!r});FW=Path({str(FW)!r});D=FW.read_bytes();B=0x40160000")
ns={'__file__':str(A/'test_native_output.py')};exec(compile(s,str(A/'test_native_output.py'),'exec'),ns)
u=ns['u'];w32=ns['w32'];w64=ns['w64'];r32=ns['r32'];r64=ns['r64'];call=ns['call'];MSG=ns['MSG'];PAY=ns['PAY'];PCM_CALLBACK=0x4e005000
observed=[]
def pcm_hook(uc,pc,size,data):
 if pc==0x40d2c448:ns['ret']();return
 if pc==PCM_CALLBACK:
  observed.append([uc.reg_read(ns['UC_ARM64_REG_X0']),uc.reg_read(ns['UC_ARM64_REG_W1']),uc.reg_read(ns['UC_ARM64_REG_W2'])]);ns['ret']()
u.hook_add(ns['UC_HOOK_CODE'],pcm_hook)
def driver_send(event,payload):
 u.mem_write(MSG,bytes(64));u.mem_write(PAY,bytes(64));u.mem_write(PAY,payload);w32(MSG+32,event);w64(MSG+40,PAY)
 call(0x40c81ec0,[MSG]);return bytes(u.mem_read(PAY,64))
rows=[]
for frames in (2400,4800,12000,24000):
 ns['reset']();observed.clear();w32(0xa02d4cb48,4);w32(0xa02d4cb50,0)
 block=frames*4;count=5;base=0x8a0000000
 cfg=struct.pack('<4I',count,block,0,0)+b'\1'+bytes(7)+struct.pack('<3Q',base,PCM_CALLBACK,0)+bytes(8)
 reply=driver_send(0x42040109,cfg);assert reply[48]==1
 assert r32(0xa02d4cbd8)==count and r32(0xa02d4cbdc)==block and r32(0xa02d4cbe4)==frames
 reply=driver_send(0x42040106,bytes([0,0,0,0,1,1,1,0]));assert reply[6]==1 and r32(0xa02d4cb48)==5
 desc=[x for x in ns['lower'] if x['event']=='0x42040709'];assert len(desc)==1,ns['lower']
 fields=struct.unpack('<QIII',bytes.fromhex(desc[0]['payload'])[:20]);assert fields==(base,block,count,frames),fields
 # Execute the actual lower ADC config dispatcher too; its state is dynamic.
 lowerpay=struct.pack('<QIII',*fields);u.mem_write(MSG,bytes(64));u.mem_write(PAY,lowerpay);w32(MSG+32,0x42040709);w64(MSG+40,PAY);call(0x40c7cfa4,[MSG])
 assert r64(0xa02d4c738)==base and r32(0xa02d4c740)==block and r32(0xa02d4c744)==count and r32(0xa02d4c750)==frames
 # Callback event contains the bytes supplied by DMA; native forwards them.
 sampleptr=base+2*block;driver_send(0x42040111,struct.pack('<IIQII',2,0,sampleptr,block,0))
 assert observed==[[sampleptr,block,2]],observed
 rows.append(dict(frames=frames,nominal_us=frames*1000000//48000,bytes=block,count=count,used_ring_bytes=count*block,descriptor709=list(fields),callback111=observed[0],native_driver_state=r32(0xa02d4cb48)))
report=dict(passed=True,firmware_sha256=hashlib.sha256(FW.read_bytes()).hexdigest(),cases=rows,boundary='Original native high/low config and callback instructions execute. OS locks/events and lower DAC/DMA execution are shims; no physical DMA cadence/IRQ stability, scheduler capacity, mic gain settle or DAC playback is proved. Shared ring address/count and native capacity are unchanged.',conclusion='500ms is the Live backend request choice, not a fixed native config/callback size. Original native API propagates 50/100/250/500ms requests to ADC descriptor and callback. Physical safety/performance still requires separate validation.')
(P/'capture-blocks.json').write_text(json.dumps(report,indent=2)+'\n');print('PASS actual G 109/106/709/111: configurable 50/100/250/500ms blocks')
