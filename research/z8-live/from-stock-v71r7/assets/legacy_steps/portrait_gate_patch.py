"""Portrait video as a switchable plugin on the user's own base (v36b 1c47e779 lineage).

The portrait layer (tkhd orientation snapshot, video OSD policy, portrait OSD layout tables)
stays in MAIN but is gated by one RAM word; the default after every boot is 0 = stock behaviour.
portrait.wlm turns it on through the existing Fenestra service `slots.state`, which gains two ops:
  13 PORTRAIT_SET  value 0/1 -> gate word      (FX_BOUNDS for anything else)
  14 PORTRAIT_GET  reply.value[0] = gate word, value[1] = magic 0x50525431 ("PRT1")
Gate word 0xa116d3480 lies in the slot reservation that boot init clears (page after the
slots limit/stage/proxy words at 0xa116d3400..0x41f, nothing else references it).

Gated sites (everything else of the portrait layer is unreachable while the gate is 0):
  0x40cf3bec  tkhd snapshot: pose read -> pose forced to 0 (identity matrix, = stock) when off
  0x4038e404  video OSD policy: suppression bit cleared only when on (stock keeps it when off)
Only x8..x10 / NZCV that the patched functions already clobber are touched; the policy gate
saves x10 because that leaf function never used it.
Finally the bootguard build ID is restamped exactly like bootguard_patch.py does.
Usage: portrait_gate_patch.py IN.main OUT.main [--input-sha SHA] [--c64label BYTES.json]
  default input: v36b 1c47e779. --input-sha accepts another bootguard-v2 MAIN of the same lineage
  (e.g. v37); --c64label applies [address, old, new] single-byte edits first (old bytes are checked).
"""
import hashlib,json,struct,subprocess,sys,tempfile
from pathlib import Path
B=0x40160000
IN_SHA='1c47e779a57dd6c2ccc88a9cb5f08312785ab5ec7c6daa7f1ef71d5287058d03'
CAVE=0x401fe200          # same RX page as the bootguard cave (0x401fe000..0x401fe0e4)
CAVE_MAX=0x100
BUILD_ID=0x401fe060
OLD_BUILD_ID=0x1586f8e4
FLAG=0xa116d3480
SITES=dict(
    SVC_BRANCH=(0x401a726c,'61310054'),   # b.ne 0x401a7898  (ops > 12 -> FX_ARGUMENT)
    SVC_RET=(0x401a7898,'f44f49a9'),      # ldp x20,x19,[sp,#0x90]  (common epilogue, returns w0)
    TKHD_POSE=(0x40cf3bec,'090140b9'),    # ldr w9,[x8]  (x8 = 0xa03228244 pose)
    TKHD_NEXT=(0x40cf3bf0,'2a050051'),    # sub w10,w9,#1
    POLICY_AND=(0x4038e404,'29791f12'),   # and w9,w9,#0xfffffffe
    POLICY_NEXT=(0x4038e408,'090100b9'),  # str w9,[x8]
)
CAVE_ASM=f"""
.globl _start
_start:
/* slots.state ops 13/14. In: x8 = op, w0 = FX_ARGUMENT(1), request copy at sp+0x10
 * (index,offset,bytes,value), x19 = reply. Out via the dispatcher's epilogue. */
svc:    cmp x8, #13
        b.eq 1f
        cmp x8, #14
        b.ne SVC_RET
        movz x9, #{FLAG&0xffff:#x}
        movk x9, #{FLAG>>16&0xffff:#x}, lsl #16
        movk x9, #{FLAG>>32&0xffff:#x}, lsl #32
        ldr w8, [x9]
        str x8, [x19, #0x10]
        movz x8, #0x5431
        movk x8, #0x5052, lsl #16
        str x8, [x19, #0x18]
        mov w0, #0
        b SVC_RET
1:      ldr w8, [sp, #0x1c]
        mov w0, #9
        cmp w8, #1
        b.hi SVC_RET
        movz x9, #{FLAG&0xffff:#x}
        movk x9, #{FLAG>>16&0xffff:#x}, lsl #16
        movk x9, #{FLAG>>32&0xffff:#x}, lsl #32
        str w8, [x9]
        dmb ish
        mov w0, #0
        b SVC_RET
/* tkhd snapshot: gate off -> pose 0 -> identity matrix (byte-identical to stock tkhd). */
tkhd:   ldr w9, [x8]
        movz x10, #{FLAG&0xffff:#x}
        movk x10, #{FLAG>>16&0xffff:#x}, lsl #16
        movk x10, #{FLAG>>32&0xffff:#x}, lsl #32
        ldr w10, [x10]
        cmp w10, #0
        csel w9, wzr, w9, eq
        b TKHD_NEXT
/* video OSD policy: gate off -> keep the stock suppression bit. */
policy: str x10, [sp, #-16]!
        movz x10, #{FLAG&0xffff:#x}
        movk x10, #{FLAG>>16&0xffff:#x}, lsl #16
        movk x10, #{FLAG>>32&0xffff:#x}, lsl #32
        ldr w10, [x10]
        cbz w10, 2f
        and w9, w9, #0xfffffffe
2:      ldr x10, [sp], #16
        b POLICY_NEXT
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
def branch(at,target,cond=None):
    off=(target-at)>>2
    if cond is None:
        assert -(1<<25)<=off<(1<<25);return struct.pack('<I',0x14000000|(off&0x3ffffff))
    assert -(1<<18)<=off<(1<<18);return struct.pack('<I',0x54000000|((off&0x7ffff)<<5)|cond)
def main(src,dst,in_sha=IN_SHA,label=None,edits=None):
    d=bytearray(Path(src).read_bytes());orig=bytes(d)
    assert hashlib.sha256(orig).hexdigest()==in_sha,'input sha mismatch'
    for name,(a,hx) in SITES.items():
        assert d[a-B:a-B+4].hex()==hx,(name,hex(a),d[a-B:a-B+4].hex())
    assert not any(d[CAVE-B:CAVE-B+CAVE_MAX]),'cave not empty'
    old_id=struct.unpack_from('<I',d,BUILD_ID-B)[0]
    assert old_id&0x01000000 and d[0x401fe068-B:0x401fe070-B]==struct.pack('<Q',0x31544f4f425846),'no bootguard v2 build id'
    if in_sha==IN_SHA:assert old_id==OLD_BUILD_ID
    labels=0
    for a,old,new in json.loads(Path(label).read_text()) if label else []:
        a=int(a,16)
        assert d[a-B]==old,(hex(a),d[a-B],old);d[a-B]=new;labels+=1
    edited=0
    for a,old,new in json.loads(Path(edits).read_text()) if edits else []:   # [address, old hex, new hex]
        a=int(a,16);old=bytes.fromhex(old);new=bytes.fromhex(new)
        assert len(old)==len(new) and d[a-B:a-B+len(old)]==old,hex(a)
        d[a-B:a-B+len(new)]=new;edited+=len(new)
    defs={k:v[0] for k,v in SITES.items()}
    cave,syms=asm(CAVE_ASM,CAVE,defs)
    assert len(cave)<=CAVE_MAX,len(cave)
    d[CAVE-B:CAVE-B+len(cave)]=cave
    patches={SITES['SVC_BRANCH'][0]:branch(SITES['SVC_BRANCH'][0],syms['svc'],cond=1),   # b.ne svc
             SITES['TKHD_POSE'][0]:branch(SITES['TKHD_POSE'][0],syms['tkhd']),
             SITES['POLICY_AND'][0]:branch(SITES['POLICY_AND'][0],syms['policy'])}
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
              c64label_bytes=labels,edit_bytes=edited,flag='%#x'%FLAG,
              cave=['%#x'%CAVE,'%#x'%(CAVE+len(cave))],symbols={k:'%#x'%v for k,v in syms.items()},
              patches={'%#x'%a:b.hex() for a,b in patches.items()},changed=[['%#x'%a,'%#x'%b] for a,b in changed])
    Path(dst+'.json').write_text(json.dumps(info,indent=2)+'\n')
    print(json.dumps(info,indent=2))
if __name__=='__main__':
    import argparse
    p=argparse.ArgumentParser();p.add_argument('src');p.add_argument('dst')
    p.add_argument('--input-sha',default=IN_SHA);p.add_argument('--c64label');p.add_argument('--edits')
    a=p.parse_args();main(a.src,a.dst,a.input_sha,a.c64label,a.edits)
