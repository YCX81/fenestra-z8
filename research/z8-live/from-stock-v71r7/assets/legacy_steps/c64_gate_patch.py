"""C64 extended Picture Control slots (C10-C64) as a switch-style plugin.

The C64 layer in MAIN only shows/serves slots below the runtime limit word 0xa116d3400:
the three virtual-key adapters (0x40b04d00/0x40b04e00/0x40b04f00), the i-menu state2 row
builder (0x40b15110) and every slots.state record op compare the slot index with it.
Boot init (0x40b20000) clears the 1 MiB slot reservation and then copies an init table
(0x40b20200 entries, data from 0x40b20608) which sets the limit to 0x40.

Switch:
  * init value 0x40 -> 9: after every boot no slot >= 9 exists = stock C1-C9 behaviour
    (the same path as the QEMU-tested limit-32 case, extended to all of C10-C64).
  * slots.state SLOTS_INFO (op 10, case entry 0x401a749c `mov w8,#0x40`): slotcard.wlm calls
    it first after opening the service. If the limit is below 32, set it to 64 and run the
    same cache invalidate (0x40b03400) that SLOTS_LIMIT uses, then continue unchanged.
    slotcard then lowers it to 32 itself when its store says so (unchanged module).
The 275 C64 label bytes (0x40b37214..) are the 55 extended slots x 5 label kinds and are only
drawn for C10+, so they need no gate. Finally the bootguard build ID is restamped.
Usage: c64_gate_patch.py IN.main OUT.main [--input-sha SHA]"""
import hashlib,json,struct,subprocess,sys,tempfile
from pathlib import Path
B=0x40160000
IN_SHA='28e11adf6ce2fde2fe365dc80d48e04a709938d8d6be1168cb6d4a48273cf7d7'
LIMIT=0xa116d3400;RES=0xa1162b000
INIT_TABLE,INIT_DATA=0x40b20200,0x40b20608
CAVE=0x401fe2c0;CAVE_MAX=0x40
BUILD_ID=0x401fe060
SITES=dict(
    INFO=(0x401a749c,'08088052'),        # mov w8,#0x40  (SLOTS_INFO: value[0] = SLOTS_ACTIVE)
    INFO_NEXT=(0x401a74a0,'e0031f2a'),   # mov w0,wzr
    INVALIDATE=(0x40b03400,'08009ad2'),  # mov x8,#0xd000  (slot cache invalidate, uses x8/x9 only)
    LIMIT_OP=(0x401a7694,'280100b9'),    # str w8,[x9]  (SLOTS_LIMIT store, same word)
    BOOT_CLEAR=(0x40b20078,'0a0096d2'),  # mov x10,#0xb000 (reservation base for the init copy)
)
CAVE_ASM=f"""
.globl _start
_start:
/* SLOTS_INFO entry. Live: x19 = reply, frame saved x29/x30 (restored by the common epilogue),
 * x8..x10 free here. Out: w8 = 0x40 as the original instruction, continue at INFO_NEXT. */
info:   movz x9, #{LIMIT&0xffff:#x}
        movk x9, #{LIMIT>>16&0xffff:#x}, lsl #16
        movk x9, #{LIMIT>>32&0xffff:#x}, lsl #32
        ldr w10, [x9]
        cmp w10, #32
        b.hs 1f
        mov w10, #64
        str w10, [x9]
        dmb ish
        bl INVALIDATE
1:      mov w8, #0x40
        b INFO_NEXT
"""
def asm(src,base,defs):
    with tempfile.TemporaryDirectory() as t:
        t=Path(t);(t/'a.s').write_text(src)
        subprocess.run(['clang','--target=aarch64-none-elf','-c',str(t/'a.s'),'-o',str(t/'a.o')],check=True)
        cmd=['ld.lld','-Ttext=%#x'%base,'-e','_start','-o',str(t/'a.elf'),str(t/'a.o')]
        for k,v in defs.items():cmd+=['--defsym','%s=%#x'%(k,v)]
        subprocess.run(cmd,check=True)
        nm=subprocess.run(['llvm-nm',str(t/'a.elf')],capture_output=True,text=True,check=True).stdout
        syms={l.split()[2]:int(l.split()[0],16) for l in nm.splitlines() if len(l.split())==3}
        subprocess.run(['llvm-objcopy','-O','binary','--only-section=.text',str(t/'a.elf'),str(t/'a.bin')],check=True)
        return (t/'a.bin').read_bytes(),syms
def branch(at,target):
    off=(target-at)>>2;assert -(1<<25)<=off<(1<<25);return struct.pack('<I',0x14000000|(off&0x3ffffff))
def limit_init_offset(d):
    """File offset of the init-table value for the limit word."""
    t=INIT_TABLE-B;data=INIT_DATA-B;i=0
    while True:
        off,n=struct.unpack_from('<II',d,t+i*8);i+=1
        if not n:raise SystemExit('limit word not in init table')
        if RES+off==LIMIT:
            assert n==8,n;return data
        data+=n
def main(src,dst,in_sha=IN_SHA):
    d=bytearray(Path(src).read_bytes());orig=bytes(d)
    assert hashlib.sha256(orig).hexdigest()==in_sha,'input sha mismatch'
    for name,(a,hx) in SITES.items():
        assert d[a-B:a-B+4].hex()==hx,(name,hex(a),d[a-B:a-B+4].hex())
    assert not any(d[CAVE-B:CAVE-B+CAVE_MAX]),'cave not empty'
    old_id=struct.unpack_from('<I',d,BUILD_ID-B)[0]
    assert old_id&0x01000000 and d[0x401fe068-B:0x401fe070-B]==struct.pack('<Q',0x31544f4f425846),'no bootguard v2 build id'
    li=limit_init_offset(d)
    assert struct.unpack_from('<Q',d,li)[0]==0x40,hex(struct.unpack_from('<Q',d,li)[0])
    struct.pack_into('<Q',d,li,9)
    cave,syms=asm(CAVE_ASM,CAVE,{k:v[0] for k,v in SITES.items()})
    assert len(cave)<=CAVE_MAX,len(cave)
    d[CAVE-B:CAVE-B+len(cave)]=cave
    patches={SITES['INFO'][0]:branch(SITES['INFO'][0],syms['info'])}
    for a,b in patches.items():d[a-B:a-B+4]=b
    struct.pack_into('<I',d,BUILD_ID-B,0)
    bid=struct.unpack('<I',hashlib.sha256(bytes(d)).digest()[:4])[0]|0x01000000
    struct.pack_into('<I',d,BUILD_ID-B,bid)
    changed=[];i=0
    while i<len(d):
        if d[i]!=orig[i]:
            j=i
            while j<len(d) and (d[j]!=orig[j] or any(d[k]!=orig[k] for k in range(j,min(j+8,len(d))))):j+=1
            changed.append([B+i,B+j]);i=j
        else:i+=1
    Path(dst).write_bytes(d)
    info=dict(source_sha256=in_sha,sha256=hashlib.sha256(d).hexdigest(),build_id='%#010x'%bid,previous_build_id='%#010x'%old_id,
              limit_word='%#x'%LIMIT,limit_init_file_offset='%#x'%li,limit_init='0x40 -> 9',
              cave=['%#x'%CAVE,'%#x'%(CAVE+len(cave))],symbols={k:'%#x'%v for k,v in syms.items()},
              patches={'%#x'%a:b.hex() for a,b in patches.items()},changed=[['%#x'%a,'%#x'%b] for a,b in changed])
    Path(dst+'.json').write_text(json.dumps(info,indent=2)+'\n')
    print(json.dumps(info,indent=2))
if __name__=='__main__':
    import argparse
    p=argparse.ArgumentParser();p.add_argument('src');p.add_argument('dst');p.add_argument('--input-sha',default=IN_SHA)
    a=p.parse_args();main(a.src,a.dst,a.input_sha)
