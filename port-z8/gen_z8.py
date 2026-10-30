"""Generate the Z8 C3.11 platform port from the Z9 C5.32 sources.

Every address goes through addrmap (verified above a similarity floor) or the explicit tables below,
each of which was established structurally (see PORT-STATUS.md). Unknown addresses abort the run.
Run: python gen_z8.py   (rewrites src/platform/z8_c311, emulation/z8_core.ld, host/build_z8_core.py
and adds the z8-c311 entries to the shared profile scripts)
"""
import hashlib, re, shutil, struct, sys
from pathlib import Path
from addrmap import Mapper, BASE, Z8

ROOT = Path(__file__).resolve().parents[1]
SHA8 = '2b43a31c9de838f84b12416de4348a7004e65be07f853ec56ec2b10f3a8e3853'
SIM_FLOOR = 0.80
FIRST8, STOP8 = 0x4bffd20, 0x68c

UNCHANGED = {0x401a0000, 0x40200000, 0xa00000000, 0xa40000000, 0x40000000, 0x40160000}
EXPLICIT = {
    # UI main loops, located by the same wait/poll edges verify_original checks
    0x403f97ec: 0x403fe284, 0x403f9844: 0x403fe2dc, 0x403fc768: 0x40401264, 0x403fc7b4: 0x404012b0,
    0x40500970: 0x40507274, 0x405009b4: 0x405072b8, 0x406120e0: 0x40616888, 0x406121a4: 0x4061694c,
    0x406143cc: 0x40618b84, 0x40614428: 0x40618be0,
    # read-only records and descriptors, from the page-array comparison
    0x403b037c: 0x403afe64,  # reviewed: entry on both sides, differences only before the entry
    # branch stubs: confirmed record by record against the editor page 678 -> 704 event table
    0x40506634: 0x4050d088, 0x40506638: 0x4050d08c, 0x40506640: 0x4050d094, 0x40506644: 0x4050d098,
    0x42271338: 0x42d93856,  # fallback font blob, 170765 bytes byte-identical on both models
    0x43014bb8: 0x43bff8e8, 0x4317e310: 0x43d595c8, 0x4317e420: 0x43d596d8, 0x4317ebe0: 0x43d59e68,
    0xa028d1a10: 0xa028ca040, 0xa028be460: 0xa028b7e40, 0x42e9b81c: 0x43a8731c, 0xa0290c7f0: 0xa029054c0,
    0x433ad2f0: 0x43f907f8,
}
EVENT = {0x295: 0x283, 0x296: 0x284, 0x297: 0x285, 0x298: 0x286, 0x29d: 0x28b, 0x2af: 0x29d, 0x2b0: 0x29e, 0x2b1: 0x29f,
         0x2b2: 0x2a0, 0x2c0: 0x2ae, 0x2c3: 0x2b1, 0x314: 0x302, 0x316: 0x304, 0x3ca: 0x3bb}
HEXRE = re.compile(r'0x([0-9a-fA-F]{8,9})(?![0-9a-fA-F])')
m = Mapper()
report = {}


def to_z8(va, where):
    if va in UNCHANGED:
        return va
    if va in EXPLICIT:
        report[va] = (EXPLICIT[va], 'explicit')
        return EXPLICIT[va]
    if 0x40160000 <= va < 0x42000000:
        r = m.code(va)
        if not r or r['sim'] < SIM_FLOOR:
            sys.exit(f'low-confidence code address {va:#x} in {where}: {r}')
        report[va] = (r['z8'], f"sim={r['sim']}")
        return r['z8']
    if va >= 0xa00000000:
        r = m.data(va)
        c = r['candidates']
        if not c or (len(c) > 1 and c[0][1] < 3 * c[1][1]):
            sys.exit(f'unresolved data address {va:#x} in {where}: {r}')
        report[va] = (c[0][0], f"refs={c[0][1]}/{r['sites']}")
        return c[0][0]
    sys.exit(f'unmapped address {va:#x} in {where}')


def map_hex(text, where, keep=lambda v: False):
    def sub(mo):
        v = int(mo.group(1), 16)
        if not (0x40000000 <= v < 0x46000000 or 0xa00000000 <= v < 0xa80000000) or keep(v):
            return mo.group(0)
        return f'{to_z8(v, where):#x}'
    return HEXRE.sub(sub, text)


def rename(text):
    return text.replace('z9_c532', 'z8_c311').replace('z9-c532', 'z8-c311').replace('fx_z9', 'fx_z8').replace('FxZ9', 'FxZ8') \
        .replace('FX_Z9', 'FX_Z8').replace('z9_debug', 'z8_debug').replace('z9-core', 'z8-core').replace('z9_core', 'z8_core') \
        .replace('build_z9', 'build_z8').replace('Nikon Z9', 'Nikon Z8').replace('C 5.32', 'C 3.11').replace('C5.32', 'C3.11').replace('C532', 'C311').replace('Z9', 'Z8').replace('-z9', '-z8')


def must(text, old, new, count=None):
    assert old in text, old
    if count is not None:
        assert text.count(old) == count, (old, text.count(old))
    return text.replace(old, new)


def port_menu(t):
    t = must(t, '#define SETUP 638u', '#define SETUP 663u')
    t = must(t, '#define TEXT 3204u', '#define TEXT 3136u')
    t = must(t, '#define LANGUAGES 29u', '#define LANGUAGES 36u')
    t = must(t, 'd->count != 45u', 'd->count != 44u')
    t = must(t, 'rows[0].word[0] != 651u || rows[42].word[0] != 906u', 'rows[0].word[0] != 640u || rows[42].word[0] != 889u')
    t = must(t, 'memcpy(s->rows + 2u, rows, 45u * sizeof(*rows));', 'memcpy(s->rows + 2u, rows, 44u * sizeof(*rows));')
    t = must(t, 'memcpy(s->events + 2u, (const void *)d->events, 45u * sizeof(uintptr_t));', 'memcpy(s->events + 2u, (const void *)d->events, 44u * sizeof(uintptr_t));')
    t = must(t, 's->setup.count = 47u;', 's->setup.count = 46u;')
    t = must(t, 'font[906],((const uint8_t *)attributes[1])[651]', 'font[889],((const uint8_t *)attributes[1])[640]')
    t = must(t, '12u,705u,656u,792u,', '12u,687u,638u,774u,')
    t = must(t, '0x6b8u', '0x68cu')
    for old, new in (('0x4becu', '0x4ba7u'), ('0x4bedu', '0x4ba8u'), ('0x4bf1u', '0x4bacu')):
        t = must(t, old, new)
    # event IDs, only in the two places that hold them
    def ev(mo):
        return f'{EVENT[int(mo.group(1), 16)]:#x}u'
    t = re.sub(r'\b0x(29[5-8]|29d|2af|2b[0-2]|2c0|2c3|314|316|3ca)u\b', ev, t)
    return t


def port_ptp(t):
    t = must(t, 'identity={532u,', 'identity={311u,')
    t = must(t, 'fx_ptp_put32(out,532u)', 'fx_ptp_put32(out,311u)')
    t = must(t, '|(1u<<(FX_PTP_LINUX_BRIDGE-1u)))', ')')
    t = must(t, 'if(op==FX_PTP_LINUX_BRIDGE)return fx_linux_request(in,bytes,out,count);\n', '')
    return t


USB_STUB = '''/* The Z9 USB Linux bridge needs a mapped shared-memory window that is not qualified on Z8.
 * It is left out: the PTP capability is not advertised and these entries are inert. */
#include "fenestra_native_linux.h"
void fx_linux_account(FxLinuxBridge *b,FxAllocationObserver o){(void)b;(void)o;}
void fx_linux_account_packets(FxLinuxBridge *b,FxIndexMemory m,FxAllocationObserver r,FxAllocationObserver d){(void)b;(void)m;(void)r;(void)d;}
void fx_linux_tick(uint64_t session,uint32_t generation,int enabled){(void)session;(void)generation;(void)enabled;}
void fx_linux_revoke(void){}
int fx_linux_quiesced(void){return 1;}
uint32_t fx_linux_ready(void){return 0u;}
'''


def gen_platform():
    src, dst = ROOT / 'src/platform/z9_c532', ROOT / 'src/platform/z8_c311'
    if dst.exists():
        shutil.rmtree(dst)
    dst.mkdir()
    for f in sorted(src.iterdir()):
        t = f.read_text(encoding='utf-8')
        if f.name == 'usb_debug.c':
            t = USB_STUB
        else:
            if f.name == 'menu.c':
                t = port_menu(t)
            if f.name == 'ptp.c':
                t = port_ptp(t)
            if f.name == 'module_stage.c':
                old = bytes.fromhex('6380ded18395ce11cb327c766721274c85127d2cf1a1e887ee60e3d156810bdd')
                new = bytes.fromhex(SHA8)
                fmt = lambda b: ','.join(f'{x:#04x}' for x in b)
                t = must(t, fmt(old[:16]), fmt(new[:16]), 1)
                t = must(t, fmt(old[16:]), fmt(new[16:]), 1)
            t = map_hex(t, f.name)
        (dst / f.name).write_text(rename(t), encoding='utf-8', newline='\n')
    ld = (ROOT / 'emulation/z9_core.ld').read_text()
    (ROOT / 'emulation/z8_core.ld').write_text(rename(ld), newline='\n')


def gen_build():
    t = (ROOT / 'host/build_z9_core.py').read_text(encoding='utf-8')
    t = t.replace("'src/native_linux.c',", '')
    t = map_hex(t, 'build_z8_core.py')
    (ROOT / 'host/build_z8_core.py').write_text(rename(t), encoding='utf-8', newline='\n')


def code8(a):
    return to_z8(a, 'profile')


def patch_profile():
    p = ROOT / 'host/platform_profiles.py'
    t = p.read_text(encoding='utf-8')
    if "'z8-c311'" in t:
        sys.exit('platform_profiles.py already patched; git checkout it first')
    z9 = t[t.index(" 'z9-c532':dict("):t.index("# Independently observed original dispatcher ABI")]
    body = z9.rstrip()
    assert body.endswith('}')
    body = body[:-1]
    d = m.d8
    kinds = {}
    for page in range(1, STOP8):
        k = struct.unpack_from('<I', d, FIRST8 + (page - 1) * 96)[0] & 0xffff
        kinds[k] = kinds.get(k, 0) + 1
    new = map_hex(body, 'profile')
    new = new.replace("'z9-c532'", "'z8-c311'").replace("model='Nikon Z9'", "model='Nikon Z8'").replace("version='C 5.32'", "version='C 3.11'")
    new = new.replace('bytes=81823629', f'bytes={len(d)}').replace('6380ded18395ce11cb327c766721274c85127d2cf1a1e887ee60e3d156810bdd', SHA8)
    new = re.sub(r'native_pages=dict\([^)]*\)', f'native_pages=dict(first={FIRST8},stop={STOP8:#x},kinds={kinds})', new)
    t = t.replace(z9, body + ',\n' + new + '}\n\n')
    # condition ABI
    win9 = 1078993640
    win8 = code8(win9)
    dig = hashlib.sha256(d[win8 - BASE:win8 - BASE + 168]).hexdigest()
    t = must(t, "'8731d7c78c916f02977eaaf3f9327f3f1484df5f58adc81465938dccc4f7a3c8')}", f"'8731d7c78c916f02977eaaf3f9327f3f1484df5f58adc81465938dccc4f7a3c8'), 'z8-c311': ({win8}, 168, '{dig}')}}")
    # card capacity
    chain = [(0x402dc99c, 0x408b94f8), (0x408b9538, 0x408bb1f8), (0x408bb248, 0x408bcd54)]
    z = ','.join(f'({code8(a):#x},{code8(b):#x})' for a, b in chain)
    t = must(t, "    else:raise ValueError('unverified native card capacity profile')",
             f"    elif target=='z8-c311':\n        chain=[{z}]\n        loop,error={code8(0x408bcda4):#x},{code8(0x408bb270):#x}\n    else:raise ValueError('unverified native card capacity profile')")
    t = must(t, "'src/platform/zr_c110/menu.c',", "'src/platform/zr_c110/menu.c','src/platform/z8_c311/menu.c',")
    p.write_text(t, encoding='utf-8', newline='\n')


def patch_sites():
    p = ROOT / 'emulation/native_ui_dispatch_sites.py'
    t = p.read_text(encoding='utf-8')
    cur = {k: code8(v) for k, v in dict(poll=0x403d2158, wait=0x403d22d4, event_wait=0x403d69f4, event_group=0x403d6908,
                                        current_task=0x401865d8, event_set=0x403d69d4, event_set_native=0x40d2fcdc,
                                        wake_reference=0x403d7c50).items()}
    # the two Z9 loops that inline memset between the flags pointer and the poll call
    setups = {}
    for poll in (0x404012b0, 0x405072b8):
        for back in range(1, 16):
            w = struct.unpack_from('<I', m.d8, poll - BASE - back * 4)[0]
            if w & 0xffc003ff == 0x910003e0:
                setups[poll] = poll - back * 4
                break
    loops = '[' + ','.join(f'({EXPLICIT[a]:#x},{EXPLICIT[b]:#x})' for a, b in (
        (0x403f97ec, 0x403f9844), (0x403fc768, 0x403fc7b4), (0x40500970, 0x405009b4), (0x406120e0, 0x406121a4), (0x406143cc, 0x40614428))) + ']'
    entry = (" 'z8-c311':dict(sha256='" + SHA8 + "',\n  " + ','.join(f'{k}={v:#x}' for k, v in cur.items()) + ',wake_mask=0x200000,\n  '
             + 'argument_setups={' + ','.join(f'{a:#x}:{b:#x}' for a, b in setups.items()) + '},\n  loops=' + loops + '),\n')
    t = must(t, " 'z9-c532':dict(", entry + " 'z9-c532':dict(", 1)
    p.write_text(t, encoding='utf-8', newline='\n')

    p = ROOT / 'emulation/native_font_sites.py'
    t = p.read_text(encoding='utf-8')
    enter = [0x402ecc3c, 0x402ecf88, 0x402ed0a0, 0x402edf50, 0x402ee098, 0x402ee1f4, 0x402ee3ac, 0x402ee5f8, 0x402ee6b4]
    entry = ("    'z8-c311': dict(\n        sha256='" + SHA8 + "',\n        prefix='fx_z8_font_', "
             f"lock={code8(0x402e4364):#x}, unlock={code8(0x402e4378):#x},\n        line_probe={code8(0x402ed94c):#x},measure={code8(0x4088f2a0):#x},set_layout={code8(0x4088ea2c):#x},\n"
             "        enter=[" + ', '.join(f'{code8(a):#x}' for a in enter) + ']),\n')
    t = must(t, "    'z9-c532': dict(", entry + "    'z9-c532': dict(", 1)
    p.write_text(t, encoding='utf-8', newline='\n')

    p = ROOT / 'host/displaced.py'
    t = p.read_text(encoding='utf-8')
    sites = {'fx_z8_system_original': 0x40d65a98, 'fx_z8_navigate_original': 0x403afa44, 'fx_z8_ptp_original_lookup': 0x407e2e0c,
             'fx_z8_ptp_original_session': 0x407e1834, 'fx_z8_ptp_original_operations': 0x4035705c, 'fx_z8_rtos_original_printf': 0x40c05714}
    entry = "'z8-c311': {" + ', '.join(f"'{k}': {code8(v)}" for k, v in sites.items()) + "}, 'z9-c532':"
    t = must(t, "'z9-c532':", entry, 1)
    p.write_text(t, encoding='utf-8', newline='\n')


if __name__ == '__main__':
    gen_platform()
    gen_build()
    patch_profile()
    patch_sites()
    out = ROOT / 'port-z8/z8-final-map.txt'
    out.write_text(''.join(f'{k:#x} {v[0]:#x} {v[1]}\n' for k, v in sorted(report.items())))
    print('mapped', len(report), 'addresses; lowest code sim',
          min((float(v[1][4:]) for v in report.values() if v[1].startswith('sim')), default=None))
