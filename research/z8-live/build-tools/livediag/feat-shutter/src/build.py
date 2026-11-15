#!/usr/bin/env python3
"""Guarded QEMU-only overlay; never constructs an updater or modifies inputs."""
import hashlib,json,subprocess,struct,argparse
from pathlib import Path
D=Path(__file__).resolve().parent
H=Path.home()/'.local/share/nikon-z8-qemu'
OUT=H/'fenestra-z8/bundle/live-a/v4'
BASE=H/'fenestra-z8/bundle/fix-20261104.main'
STOCK=Path('/mnt/c/path/to/user/Desktop/Nikon/z8-integration-20261014/.local/firmware/z8-c311-main.bin')
CORE=H/'fenestra-z8/bundle/bridge64-fccb179/tree/build/z8-core'
SRC=Path('/mnt/c/path/to/user/Desktop/Nikon/_bundle-20261102/services/fenestra-z8')
LOAD=0x40160000;START=0x401e2000;LIMIT=0x40200000
sha=lambda b:hashlib.sha256(b).hexdigest()
def run(a):subprocess.run(list(map(str,a)),check=True,cwd=D)
def target(data,site):
 w=struct.unpack_from('<I',data,site-LOAD)[0]
 assert w>>26 in (5,37),(hex(site),hex(w))
 return site+((((w&0x3ffffff)^0x2000000)-0x2000000)*4)
def main():
 OUT.mkdir(parents=True,exist_ok=True);obj=OUT/'objects';obj.mkdir(exist_ok=True)
 b=BASE.read_bytes();stock=STOCK.read_bytes()
 assert sha(b)=='c770f34014849b2c42f20b9258a32b8959253cfb296e48f83417572781e00864'
 assert sha(stock)=='2b43a31c9de838f84b12416de4348a7004e65be07f853ec56ec2b10f3a8e3853'
 assert len(b)==len(stock)
 core=(CORE/'core.bin').read_bytes();assert b[0x401a0000-LOAD:0x401a0000-LOAD+len(core)]==core
 assert not any(b[START-LOAD:LIMIT-LOAD]) and not any(stock[START-LOAD:LIMIT-LOAD])
 symbols=json.loads((CORE/'build.json').read_text())['symbols']
 names=['runtime.c','hooks.S','capture/live_capture.c','capture/native_capture.c','ui/live_ui.c',
        'codec/live_encoder.c','codec/live_mov.c','codec/live_jpeg.c']
 names+=['codec/live_transaction.c','codec/native_files.c','codec/au_ring.c','capture/live_stream.c','codec/preview_writer.c']
 objects=[];commands=[]
 for name in names:
  o=obj/(name.replace('/','_')+'.o');objects.append(o)
  flags=['--target=aarch64-none-elf','-Os','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-ffunction-sections','-fdata-sections']
  if name!='codec/live_encoder.c':flags+=['-mgeneral-regs-only','-mstrict-align']
  flags += [f'-DLA_WIDTH={args.width}',f'-DLA_HEIGHT={args.height}','-DLA_RING_CAPACITY=96','-DLA_SAMPLE_INTERVAL_US=25000']
  flags += [f'-DLIVE_BITRATE={args.bitrate}',f'-DLIVE_IDR_US={args.idr_us}',f'-DLIVE_ENCODE_SPEED={args.encode_speed}']
  flags+=['-Wall','-Wextra','-Werror','-I',D,'-I',D/'ui/core/include','-I',D/'codec/freestanding']
  if name=='codec/live_encoder.c':flags+=['-D__linux__','-Wno-unused-function','-Wno-sign-compare']
  cmd=['clang',*flags,'-c',D/name,'-o',o];run(cmd);commands.append(list(map(str,cmd)))
 exports=['fx_memset','fx_native_menu_build','fx_native_menu_link','fx_native_display_attach_global',
 'fx_native_display_detach','fx_native_display_is_current','fx_native_display_present','memcpy','memmove','memset']
 defs={k:symbols[k] for k in exports};defs['memcmp']=symbols['fx_memcmp']
 defs.update(live_old_menu_page=target(b,0x403db694),live_old_menu_text=target(b,0x4032e130),
 live_old_system_dispatch=target(b,0x40d60ec8),live_old_movie_press=target(b,0x40fdc9c8),live_irq_resume=0x40e9083c,live_old_writer=target(b,0x40ea3638),live_closed_resume=0x408b7c20,live_start_resume=0x40c26808,live_stop_resume=0x40c2681c,live_hold_resume=0x40c2682c,live_old_pool=target(b,0x40e8b328))
 linker=OUT/'payload.ld'
 linker.write_text('\n'.join(f'{k} = {v:#x};' for k,v in defs.items())+f'''
ENTRY(live_ui_system_dispatch)
SECTIONS {{ . = {START:#x}; .text : {{ *(.text*) }} .rodata : {{ *(.rodata*) }}
.data : {{ *(.data*) }} .bss : {{ *(.bss*) *(COMMON) }}
ASSERT(. <= {LIMIT:#x}, "RX padding exhausted")
ASSERT(SIZEOF(.data) == 0, "writable static data forbidden")
ASSERT(SIZEOF(.bss) == 0, "writable static state forbidden")
/DISCARD/ : {{ *(.comment) *(.note*) *(.eh_frame*) }} }}
''')
 elf=OUT/'payload.elf';blob=OUT/'payload.bin'
 run(['ld.lld','-T',linker,*objects,'-o',elf]);run(['llvm-objcopy','-O','binary',elf,blob])
 nm=subprocess.check_output(['llvm-nm','--defined-only',str(elf)],text=True)
 (OUT/'payload.nm').write_text(nm)
 sym={p[2]:int(p[0],16) for l in nm.splitlines() if len(p:=l.split())==3}
 payload=blob.read_bytes();out=bytearray(b);ranges=[]
 def patch(at,new,expected,why):
  off=at-LOAD;assert b[off:off+len(new)]==expected,(hex(at),why)
  assert all(at+len(new)<=int(r['address'],16) or at>=int(r['address'],16)+r['size'] for r in ranges)
  assert not (at<0x40a05c00 and at+len(new)>0x40a02000)
  ranges.append(dict(address=hex(at),size=len(new),purpose=why,expected=expected.hex(),replacement=new.hex()))
  out[off:off+len(new)]=new
 patch(START,payload,bytes(len(payload)),'new immutable Live payload in original RX padding')
 sites={0x403db694:('live_ui_menu_page',False),0x4032e130:('live_ui_menu_text',False),
 0x40d60ec8:('live_ui_system_dispatch',False),0x40fdc9c8:('live_press_entry',False),
 0x40e90838:('live_irq_entry',False),0x40c26804:('live_start_entry',False),0x40c26818:('live_stop_entry',False),0x40c26828:('live_hold_entry',False),0x40e8b328:('live_pool_wrap',True),0x408b7c1c:('live_closed_entry',False),0x406cd6a0:('live_finalize_wrap',True),0x406cd75c:('live_event_wrap',True),0x40ea3638:('live_writer_wrap',True),0x406cd468:('live_open_wrap',True)}
 for at,(symbol,call) in sites.items():
  expected=b[at-LOAD:at-LOAD+4]
  if at in (0x40e90838,0x406cd468,0x40c26804,0x40c26818,0x40c26828,0x40e8b328,0x408b7c1c,0x406cd6a0,0x406cd75c):assert expected==stock[at-LOAD:at-LOAD+4]
  delta=sym[symbol]-at;assert delta%4==0 and -(1<<27)<=delta<(1<<27)
  patch(at,struct.pack('<I',(0x94000000 if call else 0x14000000)|((delta//4)&0x3ffffff)),expected,symbol)
 restored=bytearray(out)
 for r in ranges:
  o=int(r['address'],16)-LOAD;restored[o:o+r['size']]=bytes.fromhex(r['expected'])
 assert restored==b
 image=OUT/'live-a.emulation-only.main';image.write_bytes(out)
 result=dict(schema='live-a-overlay/v1',input=str(BASE),input_sha256=sha(b),output=str(image),output_sha256=sha(out),
  payload_bytes=len(payload),original_core_bytes=len(core),code_start=hex(START),code_limit=hex(LIMIT),
  changed_bytes=sum(a!=z for a,z in zip(b,out)),overlap=0,reconstruction_equal=True,
  old_movie_code_unchanged=out[0x40a02000-LOAD:0x40a05c00-LOAD]==b[0x40a02000-LOAD:0x40a05c00-LOAD],
  capture_source_lease_verified=False,device_sidecars_enabled=True,flashable=False,
  encode_speed=args.encode_speed,deblocking_enabled=args.encode_speed not in (8,10),bitrate_target=args.bitrate,idr_interval_us=args.idr_us,compressed_ring_bytes=5*1024*1024,
  symbols=sym,external_symbols=defs,commands=commands,ranges=ranges,
  sources={n:sha((D/n).read_bytes()) for n in names})
 (OUT/'changed-ranges.json').write_text(json.dumps(result,indent=2)+'\n')
 args.summary_path.write_text(json.dumps({k:v for k,v in result.items() if k not in ('ranges','commands','symbols')},indent=2)+'\n')
 print(json.dumps({k:v for k,v in result.items() if k in ('output','output_sha256','payload_bytes','changed_bytes','overlap')}))
if __name__=='__main__':
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--output-dir',type=Path,default=OUT)
 parser.add_argument('--width',type=int,default=1024)
 parser.add_argument('--height',type=int,default=680)
 parser.add_argument('--encode-speed',type=int,choices=range(11),default=9)
 parser.add_argument('--summary-path',type=Path,default=D/'build-summary.json')
 parser.add_argument('--bitrate',type=int,default=8000000)
 parser.add_argument('--idr-us',type=int,default=500000)
 args=parser.parse_args()
 assert 1000000<=args.bitrate<=8000000
 assert 100000<=args.idr_us<=1000000
 assert args.output_dir.resolve().is_relative_to(OUT.resolve())
 OUT=args.output_dir.resolve()
 main()
