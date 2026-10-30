"""Side-by-side differing instructions around each mapped address that is not an obvious match."""
import json, struct, sys
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from addrmap import Mapper, BASE
md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
def dis(w, pc):
    for i in md.disasm(struct.pack('<I', int(w)), pc): return f'{i.mnemonic} {i.op_str}'
    return f'.word {int(w):#x}'
m = Mapper(); M = json.load(open('z8-map.json'))
mode = sys.argv[1] if len(sys.argv) > 1 else 'review'
for k, e in M.items():
    va = int(k, 16)
    if e['status'] != mode or 'sim' not in e: continue
    z8 = int(e['z8'], 16); i = (va-BASE)//4; j = (z8-BASE)//4
    pre9 = int(m.w9[i-1]); pre8 = int(m.w8[j-1])
    entry = lambda w: w == 0xd65f03c0 or w>>26 == 5 or w == 0
    print(f"\n{k} -> {e['z8']} sim={e['sim']} votes={e['votes']}/{e['second']} entry={entry(pre9)}/{entry(pre8)} used={e['used_in'][0]}")
    for o in range(-8, 12):
        if m.n9[i+o] != m.n8[j+o]:
            print(f"  {o:+3d} {dis(m.w9[i+o], va+4*o):40s} | {dis(m.w8[j+o], z8+4*o)}")
