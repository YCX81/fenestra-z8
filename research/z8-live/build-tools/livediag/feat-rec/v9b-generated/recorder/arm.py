#!/usr/bin/env python3
"""lamp-v6 offline layout by default. --usb explicitly connects; --token publishes skip_nr before arm; default skip_nr=0.
This utility was not run against hardware. USB changes camera lifecycle.
"""
import argparse, io, json, struct, sys
from pathlib import Path
from elftools.elf.elffile import ELFFile

def layout(path):
 e=ELFFile(io.BytesIO(path.read_bytes()))
 s=next(s for s in e.get_section_by_name('.symtab').iter_symbols() if s.name=='fr_layout')
 sec=e.get_section(s['st_shndx']);off=s['st_value']-sec['sh_addr']
 size,rec,n,tx,epoch,shutdown,file_bytes,slots=struct.unpack_from('<8I',sec.data(),off)
 extra={}
 if s['st_size']>=44:
  skip,active,magic=struct.unpack_from('<3I',sec.data(),off+32)
  if magic!=0x4e523031:raise ValueError('NR layout version')
  extra=dict(skip_nr_offset=skip,live_exit_active_offset=active,nr_layout_magic=magic)
 if s['st_size']>=48:
  v6,=struct.unpack_from('<I',sec.data(),off+44)
  if v6!=0x4c503036:raise ValueError('lamp-v6 layout version')
  extra.update(lamp_version=6,prepare_only=True,report_mode='close checkpoints; no periodic watchdog',short_code_fps=120)
 return dict(**extra,runtime_size=size,recorder_offset=rec,recorder_size=n,tx_offset=tx,card_epoch_offset=epoch,shutdown_offset=shutdown,file_bytes=file_bytes,transaction_slots=slots)
def read_runtime_status(read,eng,l):
 return tuple(struct.unpack('<I',read(eng+l[k],4))[0] for k in ('card_epoch_offset','shutdown_offset'))
def write_arm(read,write,addr,l,token,skip_nr=False):
 if not 0<=token<=99999 or (skip_nr and not token):raise ValueError('skip_nr requires positive token')
 if l.get('lamp_version')!=6:raise ValueError('lamp-v6 layout required; no writes')
 def u32(off):return struct.unpack('<I',read(addr+off,4))[0]
 # Reject all writes while a session/prepare/EXIT is active, including disarm:
 # prevents clearing an owner marker or flipping the switch between the pair.
 if u32(12) or u32(16) or u32(96) or struct.unpack('<Q',read(addr+l['live_exit_active_offset'],8))[0]:
  raise ValueError('claimed/busy/live EXIT: no write')
 if token and token<=u32(4):raise ValueError('old token: no write')
 write(addr,struct.pack('<I',0))
 write(addr+l['skip_nr_offset'],struct.pack('<I',int(skip_nr)))
 if u32(l['skip_nr_offset'])!=int(skip_nr):raise RuntimeError('skip_nr readback failed; arm remains zero')
 write(addr,struct.pack('<I',token))
 if u32(0)!=token:raise RuntimeError('arm readback failed')

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('layout_object',type=Path)
 p.add_argument('--plan',action='store_true',help='offline ordered RAM write plan; never connects');p.add_argument('--usb',action='store_true');p.add_argument('--helpers',type=Path)
 p.add_argument('--skip-nr',action='store_true');p.add_argument('--token',type=int);a=p.parse_args();l=layout(a.layout_object);print(json.dumps(l,indent=2))
 if a.token is not None and not 0<=a.token<=99999:p.error('--token range 0..99999')
 if a.skip_nr and not a.token:p.error('--skip-nr requires a positive --token')
 if a.plan:
  if a.usb:p.error('--plan and --usb are exclusive')
  if a.token is None or not 0<=a.token<=99999:p.error('--plan requires --token 0..99999')
  if l.get('lamp_version')!=6:p.error('lamp-v6 layout required')
  print(json.dumps(dict(offline_only=True,report_mode=l['report_mode'],video_fps=120,preconditions='Exact matching lamp-v6 image; idle photo mode; no claimed/lock/entry_claimed/live_exit_active; new token; read back each write; release-publish arm last',
   relative_to='fr_state',writes=[dict(offset=0,value=0),dict(offset=l['skip_nr_offset'],value=int(a.skip_nr)),dict(offset=0,value=a.token)]),indent=2))
  return
 if not a.usb:
  if a.token is not None:p.error('--token requires explicit --usb')
  return
 if a.helpers is None:p.error('--usb requires --helpers directory containing livectl.py')
 sys.path.insert(0,str(a.helpers.resolve()));import livectl
 t,probe=livectl.connect()
 try:
  def q(at):return struct.unpack('<Q',probe.debug.read(at,8))[0]
  def ptr(at):
   v=q(at)
   if not 0xa00000000<=v<0x1000000000 or v&7:raise RuntimeError('invalid runtime pointer')
   return v
  root=ptr(0xa032b50c8+16);core=ptr(root+663*96+80)
  if struct.unpack('<I',probe.debug.read(core+64,4))[0]!=0x465a394d:raise RuntimeError('core identity')
  reg=ptr(core+2528);ui=ptr(reg+8)
  if struct.unpack('<I',probe.debug.read(ui,4))[0]!=0x4c505549:raise RuntimeError('UI identity')
  eng=ptr(ui+8)
  if struct.unpack('<II',probe.debug.read(eng,8))!=(0x4c415231,l['runtime_size']):raise RuntimeError('runtime ABI mismatch')
  addr=eng+l['recorder_offset'];state=struct.unpack('<16I',probe.debug.read(addr,64))
  tx=struct.unpack('<'+str(l['transaction_slots']*6)+'I',probe.debug.read(addr+l['tx_offset'],l['transaction_slots']*24))
  epoch,shutdown=read_runtime_status(probe.debug.read,eng,l)
  print('runtime_epoch',epoch,'shutdown',shutdown,'prepared_epoch',state[11],'epoch_changed',epoch!=state[11])
  print('runtime',hex(eng),'arm',hex(addr),'state',state,'transactions',tx)
  if a.token is not None:
   if not 0<=a.token<=99999:raise RuntimeError('token range 0..99999')
   if a.token and (state[3] or state[4] or struct.unpack('<I',probe.debug.read(addr+96,4))[0] or a.token<=state[1]):raise RuntimeError('claimed/busy/old token: no write')
   if a.token and struct.unpack('<I',probe.debug.read(0xa03bd062c,4))[0]!=0:raise RuntimeError('prepare requires photo mode')
   write_arm(probe.debug.read,probe.debug.write,addr,l,a.token,a.skip_nr)
   print('arm readback',struct.unpack('<I',probe.debug.read(addr,4))[0],'; wait for ready before REC')
 finally:t.__exit__(None,None,None)
if __name__=='__main__':main()
