#!/usr/bin/env python3
"""Offline layout by default. --usb explicitly connects; --token writes ONLY arm.
This utility was not run against hardware. USB changes camera lifecycle.
"""
import argparse, io, json, struct, sys
from pathlib import Path
from elftools.elf.elffile import ELFFile

def layout(path):
 e=ELFFile(io.BytesIO(path.read_bytes()))
 s=next(s for s in e.get_section_by_name('.symtab').iter_symbols() if s.name=='fr_layout')
 sec=e.get_section(s['st_shndx']);off=s['st_value']-sec['sh_addr']
 size,rec,n,tx,epoch=struct.unpack_from('<5I',sec.data(),off)
 return dict(runtime_size=size,recorder_offset=rec,recorder_size=n,tx_offset=tx,card_epoch_offset=epoch)
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('layout_object',type=Path)
 p.add_argument('--usb',action='store_true');p.add_argument('--helpers',type=Path)
 p.add_argument('--token',type=int);a=p.parse_args();l=layout(a.layout_object);print(json.dumps(l,indent=2))
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
  tx=struct.unpack('<24I',probe.debug.read(addr+l['tx_offset'],96))
  print('runtime',hex(eng),'arm',hex(addr),'state',state,'transactions',tx)
  if a.token is not None:
   if not 0<=a.token<=99999:raise RuntimeError('token range 0..99999')
   if a.token and (state[3] or state[4] or a.token<=state[1]):raise RuntimeError('claimed/busy/old token: no write')
   if a.token and struct.unpack('<I',probe.debug.read(0xa03bd062c,4))[0]!=0:raise RuntimeError('prepare requires photo mode')
   probe.debug.write(addr,struct.pack('<I',a.token))
   print('arm readback',struct.unpack('<I',probe.debug.read(addr,4))[0],'; wait for ready before REC')
 finally:t.__exit__(None,None,None)
if __name__=='__main__':main()
