#!/usr/bin/env python3
"""Execute original SFX resource initialization/getters on G and F.
The already-loaded resource pointer is a fixture; its zero body follows the
actual QEMU loader manifest. No resource lookup, init or getter is shimmed.
"""
from pathlib import Path
import hashlib,json,struct
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM
from unicorn.arm64_const import *
P=Path(__file__).resolve().parent; B=0x40160000
images={'G':P.parents[2]/'v52g.main','F':P/'v53f.main'}
# Resolve root path explicitly (review->production->shutter->portrait).
images['G']=P.parents[2]/'v52g.main'
RES=0xa15aab8b8; DESC=0xa0291c948+0x4e*48; BANK=0xa0291dab8+24*40
rows=[]
for name,path in images.items():
 d=path.read_bytes();u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(B,(len(d)+4095)&~4095);u.mem_write(B,d)
 for a,n in [(0x4e000000,0x10000),(0xa02910000,0x10000),(0xa038fe000,0x2000),(RES&~4095,0x11000)]:u.mem_map(a,n)
 for addr,n in [(DESC,48),(BANK,40)]:u.mem_write(addr,d[addr-0xa02808000+0x4b13000:addr-0xa02808000+0x4b13000+n])
 u.mem_write(DESC+16,struct.pack('<Q',RES));u.mem_write(DESC+24,struct.pack('<I',0xee00))
 assert struct.unpack('<Q',u.mem_read(BANK,8))[0]==0x46460000
 def call(pc,arg=0,end=0x4e001000):
  u.reg_write(UC_ARM64_REG_SP,0x4e00f000);u.reg_write(UC_ARM64_REG_LR,end);u.reg_write(UC_ARM64_REG_X0,arg)
  u.emu_start(pc,end,count=10000);assert u.reg_read(UC_ARM64_REG_PC)==end
  return u.reg_read(UC_ARM64_REG_X0)
 for n in (0,4800,30720):
  body=bytearray(0xee00);struct.pack_into('<IH',body,0x1c,0 if n==0 else 0x40,n)
  u.mem_write(RES,bytes(body));call(0x40e34184,end=0x40e342e0)
  length=call(0x40e348f4,2);ptr=call(0x40e3469c,2)
  assert length==n and ptr==RES+(0 if n==0 else 0x40)
  assert call(0x40e348f4,1)==4800 and call(0x40e3469c,1)==0xa15c19f00
  rows.append(dict(image=name,resource_header_length=n,tone2_length=length,tone2_pointer=hex(ptr),tone1_length=4800))
g=images['G'].read_bytes();f=images['F'].read_bytes()
ranges=[(0x40d6b460,0x40d6b4e0),(0x40d6c53c,0x40d6c628),(0x40d712ac,0x40d712fc),(0x40e34184,0x40e342e0),(0x40e3469c,0x40e349e8)]
for a,b in ranges:assert g[a-B:b-B]==f[a-B:b-B]
result=dict(passed=True,images={k:hashlib.sha256(v.read_bytes()).hexdigest()for k,v in images.items()},cases=rows,resource=hex(0x4e),bank=24,source_rom='0x46460000',declared_bytes=0xee00,original_init='0x40e34184',tone2_offset='header+0x1c',tone2_length='u16(header+0x20)',runtime_length='0xa038feb10',unchanged_native_ranges=[[hex(a),hex(b)]for a,b in ranges],boundary='RAM resource descriptor +16 is initialized to the observed loaded resource pointer as fixture. Original 40d6b460/40d6c53c/40d712c8/40d712ac and init/getters execute. Zero source header follows current loader absence; this does not itself read live RAM before SFX or prove no subsequent overwrite. Nonzero headers are explicit synthetic controls, not recovered sound assets.')
(P/'sfx-resource-arm.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS G/F original init/getters: zero source gives tone2 zero before any sound request; 2 nonzero controls correct')
