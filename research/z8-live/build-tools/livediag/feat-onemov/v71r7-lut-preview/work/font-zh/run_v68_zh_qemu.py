#!/usr/bin/env python3
"""Full-system QEMU visual + DRAM verification of V67 Chinese font engine & UI localization.

Boots v67.main with updated FENESTRA/{portrait.wlm, slotcard.wlm, STARTUP-Z8.0},
verifies CJK_EXT_1 (0x408c6000) & CJK_EXT_2 (0x40b8a000) loaded intact into DRAM,
switches UI language to Simplified Chinese (0xa02f2ab70 = 8), waits for both WLMs
to poll SLOTS_LANG_GET (op 15), then navigates and screenshots:
  1. Setup -> Fenestra (Chinese core + Live Photo + Live playback key)
  2. Live Photo submenu (开启 / 关闭 / 快门声音: 开启)
  3. Live playback key submenu (实况回放按键 / REC / 关闭)
  4. Setup -> WonderLantern Apps list (竖屏录像 / C64 滤镜槽)
  5. First WLM page (portrait or slotcard)
  6. Second WLM page (slotcard or portrait)
"""
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path
from types import SimpleNamespace

BASE = Path.home() / '.local/share/nikon-z8-qemu'
SNAP = BASE / 'live-regress/snap'
ROOT = SNAP / 'project'
DRIVER_DIR = BASE / 'live-audio/regress/rerun-w10/retry-driver'
STOCK = Path.home() / 'Nikon/z8-qemu/.local/firmware/z8-c311-main.bin'

sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(DRIVER_DIR))

import z8_capture_probe as probe
import pipe_transport
from PIL import Image
import virtual_recording_card as vrc
import exfat_put
import vrc_extents_helper

_init = pipe_transport.PipeIO.__init__
def bounded_init(self, base, timeout=2):
    _init(self, base, timeout=timeout)
pipe_transport.PipeIO.__init__ = bounded_init
pipe_transport.install()


def sha_bytes(b):
    return hashlib.sha256(b).hexdigest()


def sha_file(p):
    h = hashlib.sha256()
    with Path(p).open('rb') as f:
        while chunk := f.read(1024 * 1024):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--name', required=True)
    ap.add_argument('--main', type=Path, required=True)
    ap.add_argument('--fenestra', type=Path, required=True)
    ap.add_argument('--v68-json', type=Path, required=True)
    ap.add_argument('--out-dir', type=Path, required=True)
    ap.add_argument('--seconds', type=int, default=300)
    ap.add_argument('--qemu', type=Path, default=BASE / 'merged/work/build/qemu-system-aarch64')
    args = ap.parse_args()

    meta = json.loads(args.v68_json.read_text())
    out = args.out_dir
    if out.exists():
        shutil.rmtree(out)
    out.parent.mkdir(parents=True, exist_ok=True)

    os.environ['TMPDIR'] = str(out.parent)
    os.environ['FX_ICOUNT_SLEEP'] = 'off'

    # Inject our updated FENESTRA folder onto virtual CFexpress card
    _create = vrc.create
    def card_create(path, fs='blank'):
        result = _create(path, fs)
        if fs == 'exfat':
            result['recopied_extents'] = vrc_extents_helper.recopy(path, result)
        files = {p.name: p.read_bytes() for p in sorted(args.fenestra.iterdir()) if p.is_file()}
        result['fenestra_card'] = exfat_put.put(path, 'FENESTRA', files)
        result['fenestra_files'] = sorted(files)
        return result
    vrc.create = card_create

    def _extents_separate_fd(stream, length):
        fd = os.open(f'/proc/self/fd/{stream.fileno()}', os.O_RDONLY)
        pos = 0
        try:
            while pos < length:
                try:
                    start = os.lseek(fd, pos, os.SEEK_DATA)
                except OSError:
                    return
                end = min(length, os.lseek(fd, start, os.SEEK_HOLE))
                yield start, end
                pos = end
        finally:
            os.close(fd)
    vrc.extents = _extents_separate_fd

    # Helper to read current Nikon menu descriptor (same as run_portrait.py)
    def valid(a, n=8):
        return 0xA00000000 <= a <= 0xA40000000 - n

    def rd(g, a, n):
        return int.from_bytes(g.read_bytes(a, n), 'little')

    def menu_state(g):
        stack = rd(g, 0xA032B5160, 8)
        if not valid(stack, 24):
            return {'page': 0}
        stride, count, data = rd(g, stack, 4), rd(g, stack + 8, 4), rd(g, stack + 16, 8)
        if not (8 <= stride <= 256 and 1 <= count <= 64 and valid(data, count * stride)):
            return {'page': 0}
        page = rd(g, data + (count - 1) * stride, 8)
        if not valid(page, 96):
            return {'page': 0}
        identity = rd(g, page, 4)
        descriptor = rd(g, page + 80, 8)
        res = dict(page=hex(page), page_id=identity >> 16, selected=rd(g, page + 50, 2) + rd(g, page + 88, 2))
        if valid(descriptor, 64):
            rows_va = rd(g, descriptor + 16, 8)
            size = rd(g, descriptor + 32, 2)
            idx = res['selected']
            res['count'] = size
            if (identity >> 16) == 25 and res['selected'] > 0:
                res['selected'] -= 1
            if 0 < size <= 256 and (valid(rows_va, size * 44) or 0x40160000 <= rows_va <= 0x45000000 - size * 44):
                raw = g.read_bytes(rows_va, size * 44)
                res['rows'] = [
                    dict(index=i,
                         text_id=int.from_bytes(raw[i * 44:i * 44 + 4], 'little'),
                         row_id=int.from_bytes(raw[i * 44 + 24:i * 44 + 28], 'little'))
                    for i in range(size)
                ]
                if idx < size:
                    res.update({k: v for k, v in res['rows'][idx].items() if k != 'index'})
            hdr = rd(g, descriptor + 8, 8)
            if 0x40160000 <= hdr < 0x45000000 or valid(hdr, 72):
                res['title_id'] = rd(g, hdr + 68, 4)
        return res

    def direction(menu, want_idx):
        cur, cnt = menu['selected'], menu['count']
        if cur == want_idx:
            return 'ok'
        return 'down' if (want_idx - cur) % cnt <= (cur - want_idx) % cnt else 'up'

    # State machine for menu tour in Chinese!
    ST = {
        'dram_verified': False,
        'lang_set': False,
        'phase': 'wait_setup',   # wait_setup -> fenestra -> live -> pb -> wonder -> mod0 -> mod1 -> done
        'shots': {},
        'font_slots_zh': None,
        'dram_checks': {},
        'portrait_gate': None,
        'wl_mod_indices': [],
        'wl_mod_pos': 0,
    }
    LIVE_CONTACTS = []

    def save_shot(png_src: Path, tag: str, menu, g):
        dst = out / f'{tag}.png'
        shutil.copy2(png_src, dst)
        eng = rd(g, 0xA02E8CA90, 8)
        slots = [rd(g, eng + 232 + i * 8, 8) for i in range(4)] if valid(eng, 264) else []
        slot_roms = [rd(g, s + 24, 8) if valid(s, 40) else 0 for s in slots]
        lang = rd(g, 0xA02F2AB70, 4)
        ST['shots'][tag] = dict(
            file=dst.name,
            lang=lang,
            page_id=hex(menu.get('page_id', 0)),
            title_id=menu.get('title_id'),
            rows_count=menu.get('count', 0),
            selected=menu.get('selected'),
            engine=hex(eng),
            slots=[hex(x) for x in slots],
            slot_roms=[hex(x) for x in slot_roms],
        )
        print(f"SHOT {tag}: lang={lang} page={hex(menu.get('page_id',0))} slot_roms={[hex(x) for x in slot_roms]}", flush=True)
        if lang == 8 and any(slot_roms):
            ST['font_slots_zh'] = [hex(x) for x in slot_roms]

    def verify_dram_and_set_zh(g):
        if not ST['dram_verified']:
            # Verify CJK_EXT_1, CJK_EXT_2, overlay loaded intact in live QEMU DRAM!
            c1 = g.read_bytes(int(meta['cjk_ext_1']['va'], 16), meta['cjk_ext_1']['bytes'])
            c2 = g.read_bytes(int(meta['cjk_ext_2']['va'], 16), meta['cjk_ext_2']['bytes'])
            ok1 = sha_bytes(c1) == meta['cjk_ext_1']['sha256']
            ok2 = sha_bytes(c2) == meta['cjk_ext_2']['sha256']
            ST['dram_checks'] = dict(
                cjk1_ok=ok1, cjk1_sha=sha_bytes(c1),
                cjk2_ok=ok2, cjk2_sha=sha_bytes(c2),
                portrait_gate=rd(g, 0xA116D3480, 4),
                slots_limit=rd(g, 0xA116D3400, 4),
            )
            print("DRAM_VERIFY", json.dumps(ST['dram_checks']), flush=True)
            assert ok1, "CJK_EXT_1 DRAM mismatch!"
            assert ok2, "CJK_EXT_2 DRAM mismatch! Loader copy-end bump failed!"
            ST['dram_verified'] = True
        if not ST['lang_set']:
            # Switch Nikon UI language to Simplified Chinese (8)!
            g.qt_command('write 0xa02f2ab70 0x4 0x08000000')
            got_lang = rd(g, 0xA02F2AB70, 4)
            print(f"LANG_SWITCH: 0xa02f2ab70 = {got_lang}", flush=True)
            assert got_lang == 8
            ST['lang_set'] = True

    def drive_step(g, label, png_path):
        item = next((x for x in LIVE_CONTACTS if label.endswith('-' + x['label'])), None)
        if not item:
            return
        kind = item['label']
        # Early wake step: check DRAM and switch language to Chinese BEFORE opening menu!
        if kind == 'wake-release':
            verify_dram_and_set_zh(g)
            return
        if kind.endswith('-release') or not kind.startswith('tour-'):
            return

        menu = menu_state(g)
        page = menu.get('page_id', 0)
        rows = menu.get('rows', [])
        ST['portrait_gate'] = rd(g, 0xA116D3480, 4)
        key = None
        phase = ST['phase']

        if phase == 'wait_setup':
            if page == 1:
                key = direction(menu, 4)
                if key == 'ok':
                    key = 'right'
            elif page == 663:
                if 'shot_00_setup_zh' not in ST['shots']:
                    save_shot(png_path, 'shot_00_setup_zh', menu, g)
                # Find Fenestra row (text_id == 3154 or row_id == 0x7ff0)
                idx = next((r['index'] for r in rows if r['text_id'] == 3154 or r['row_id'] == 0x7FF0), None)
                if idx is not None:
                    key = direction(menu, idx)
                    if key == 'ok':
                        ST['phase'] = 'fenestra'
            elif page:
                key = 'down'

        elif phase == 'fenestra':
            if page == 0x7000:
                if 'shot_01_fenestra_zh' not in ST['shots']:
                    save_shot(png_path, 'shot_01_fenestra_zh', menu, g)
                # Navigate to Live Photo (text_id == 3177 or row_id == 0x7fb2)
                idx = next((r['index'] for r in rows if r['text_id'] == 3177 or r['row_id'] == 0x7FB2), None)
                if idx is not None:
                    key = direction(menu, idx)
                    if key == 'ok':
                        ST['phase'] = 'live'
            elif page == 663:
                idx = next((r['index'] for r in rows if r['text_id'] == 3154 or r['row_id'] == 0x7FF0), None)
                if idx is not None:
                    key = direction(menu, idx)

        elif phase == 'live':
            if page == 0x705E:
                save_shot(png_path, 'shot_02_live_photo_zh', menu, g)
                key = 'left'
                ST['phase'] = 'to_pb'
            elif page == 0x7000:
                idx = next((r['index'] for r in rows if r['text_id'] == 3177 or r['row_id'] == 0x7FB2), None)
                if idx is not None:
                    key = direction(menu, idx)

        elif phase == 'to_pb':
            if page == 0x7000:
                # Find Live playback key (text_id == 3160)
                idx = next((r['index'] for r in rows if r['text_id'] == 3160), None)
                if idx is not None:
                    key = direction(menu, idx)
                    if key == 'ok':
                        ST['phase'] = 'pb'
                else:
                    # fallback: leave to wonderlantern
                    key = 'left'
                    ST['phase'] = 'to_wonder'
            elif page == 0x705E:
                key = 'left'

        elif phase == 'pb':
            if page != 0x7000 and page != 663 and page != 0:
                save_shot(png_path, 'shot_03_live_pb_key_zh', menu, g)
                key = 'left'
                ST['phase'] = 'to_wonder_from_pb'
            elif page == 0x7000:
                idx = next((r['index'] for r in rows if r['text_id'] == 3160), None)
                if idx is not None:
                    key = direction(menu, idx)

        elif phase == 'to_wonder_from_pb':
            if page == 0x7000:
                key = 'left'
                ST['phase'] = 'to_wonder'
            elif page == 663:
                ST['phase'] = 'to_wonder'
            else:
                key = 'left'

        elif phase == 'to_wonder':
            if page == 663:
                # Find WonderLantern (text_id == 3155)
                idx = next((r['index'] for r in rows if r['text_id'] == 3155), None)
                if idx is not None:
                    key = direction(menu, idx)
                    if key == 'ok':
                        ST['phase'] = 'wonder'
            elif page == 0x7000:
                key = 'left'

        elif phase == 'wonder':
            if page != 663 and page != 0x7000 and page != 0:
                if 'shot_04_wonderlantern_zh' not in ST['shots']:
                    save_shot(png_path, 'shot_04_wonderlantern_zh', menu, g)
                    # Record all non-Back module rows (text_id >= 3254)
                    ST['wl_page'] = page
                    ST['wl_mod_indices'] = [r['index'] for r in rows if r['text_id'] >= 3254]
                    ST['wl_mod_pos'] = 0
                    print(f"WONDER_ROWS: page={hex(page)} rows={rows} mod_indices={ST['wl_mod_indices']}", flush=True)
                if ST['wl_mod_pos'] < len(ST['wl_mod_indices']):
                    target_i = ST['wl_mod_indices'][ST['wl_mod_pos']]
                    key = direction(menu, target_i)
                    if key == 'ok':
                        ST['phase'] = 'in_mod'
                else:
                    ST['phase'] = 'done'
                    (out / 'stop.request').write_text('done\n')
            elif page == 663:
                idx = next((r['index'] for r in rows if r['text_id'] == 3155), None)
                if idx is not None:
                    key = direction(menu, idx)

        elif phase == 'in_mod':
            if page != ST.get('wl_page') and page != 663 and page != 0:
                pos = ST['wl_mod_pos']
                ST['mod_outer_page'] = page
                save_shot(png_path, f'shot_0{5 + pos}_wlm_mod{pos}_outer_zh', menu, g)
                # Enter the inner FX_UI_PAGE (index 0) to see all inner rows!
                key = 'ok'
                ST['phase'] = 'in_mod_inner'
            elif page == ST.get('wl_page'):
                target_i = ST['wl_mod_indices'][ST['wl_mod_pos']]
                key = direction(menu, target_i)

        elif phase == 'in_mod_inner':
            if page != ST.get('mod_outer_page') and page != ST.get('wl_page') and page != 0:
                pos = ST['wl_mod_pos']
                save_shot(png_path, f'shot_0{5 + pos}b_wlm_mod{pos}_inner_zh', menu, g)
                # Test interactive OK toggle on row 0 (portrait On->Off) or row 2 (slotcard C64->C32)!
                if pos == 0:
                    key = 'ok'
                    ST['phase'] = 'in_mod_toggled'
                else:
                    # slotcard: move down to row 2 (R_LIMIT) and toggle C64 -> C32 (重启生效)!
                    idx_lim = min(2, max(0, menu.get('count', 1) - 2))
                    key = direction(menu, idx_lim)
                    if key == 'ok':
                        ST['phase'] = 'in_mod_toggled'
            elif page == ST.get('mod_outer_page'):
                key = 'ok'

        elif phase == 'in_mod_toggled':
            pos = ST['wl_mod_pos']
            save_shot(png_path, f'shot_0{5 + pos}c_wlm_mod{pos}_toggled_zh', menu, g)
            ST['wl_mod_pos'] += 1
            key = 'left'
            ST['phase'] = 'leave_mod_inner'

        elif phase == 'leave_mod_inner':
            if page == ST.get('wl_page'):
                ST['phase'] = 'wonder'
                if ST['wl_mod_pos'] < len(ST['wl_mod_indices']):
                    target_i = ST['wl_mod_indices'][ST['wl_mod_pos']]
                    key = direction(menu, target_i)
                    if key == 'ok':
                        ST['phase'] = 'in_mod'
                else:
                    ST['phase'] = 'done'
                    (out / 'stop.request').write_text('done\n')
            else:
                key = 'left'

        elif phase == 'done':
            (out / 'stop.request').write_text('done\n')

        if key:
            body, expected = probe.navigation_contact(key)
        else:
            body, expected = probe.key_body(), probe.expected_keys()
        item['body'], item['expected'] = body, expected
        if not key:
            item['gap'] = 10000000
            rel = next((x for x in LIVE_CONTACTS if x['label'] == kind + '-release'), None)
            if rel:
                rel['gap'] = 10000000
        print(f"STEP {kind} phase={ST['phase']} page={hex(page)} key={key}", flush=True)

    class Guest(probe.PreparedGuest):
        def __init__(self, *a, **kw):
            super().__init__(*a, **kw)
            self.command[self.command.index('-kernel') + 1] = str(args.main.resolve())
            at = self.command.index('-icount') + 1
            self.command[at] = self.command[at].replace('sleep=on', 'sleep=off')
            self.data['v68_zh_ui'] = dict(main=str(args.main), main_sha256=sha_file(args.main))

        def __enter__(self):
            g = super().__enter__()
            orig_cmd = g.qmp.command

            def watchdog():
                deadline = time.monotonic() + args.seconds
                while g.proc.poll() is None:
                    if (g.out / 'stop.request').exists() or time.monotonic() > deadline:
                        g.proc.terminate()
                        return
                    time.sleep(0.5)
            threading.Thread(target=watchdog, daemon=True).start()

            def command(name, **kw):
                res = orig_cmd(name, **kw)
                if name == 'screendump':
                    p = Path(kw['filename'])
                    png = p.with_suffix('.png')
                    Image.open(p).save(png)
                    p.unlink()
                    drive_step(g, p.stem, png)
                return res
            g.qmp.command = command
            return g

    probe.PreparedGuest = Guest
    class Peer(probe.CapturePeer):
        def __init__(self, *a, **kw):
            super().__init__(*a, **kw)
            self.trace_points = {}
    probe.CapturePeer = Peer
    probe.MENU_CONTACTS['ok'] = (0x2A, 0x8000, 0)

    def contacts(_args):
        nonlocal LIVE_CONTACTS
        rows = []
        def shutter(label, half=False, full=False, gap=1.0):
            rows.append(dict(label=label, body=probe.shutter_body(half, full),
                             expected=probe.expected_keys(half, full), gap=int(gap * 1e9), half=half, full=full))
        def key(label, k, gap=0.5):
            body, expected = probe.navigation_contact(k)
            rows.append(dict(label=label, body=body, expected=expected, gap=400000000, half=False, full=False))
            rows.append(dict(label=label + '-release', body=probe.key_body(), expected=probe.expected_keys(),
                             gap=int(gap * 1e9), half=False, full=False))

        shutter('wake-half', True, gap=1.0)
        shutter('wake-release', gap=4.0)   # 4s wait after switching to zh-CN so portrait/slotcard poll SLOTS_LANG_GET!
        key('ui-menu', 'menu', 6.0)
        key('ui-left', 'left', 0.8)
        for i in range(65):
            key(f'tour-{i:02d}', 'down', 0.6)
        LIVE_CONTACTS = rows
        return rows, len(rows)

    probe.contact_sequence = contacts

    recipe = json.loads((SNAP / 'bundle.json').read_text())['recipe']
    names = {s.split('=')[0] for s in probe.load_model_profile()['properties']}
    names |= {'pcie-link-diagnostic-mask', 'pcie-dbi-model', 'pcie-ram-dma-input',
              'pcie-image-ram-dma-input', 'pcie-intx-model', 'sensor-frame-tag-model'}
    extra = [s for s in recipe['properties'] if s.split('=')[0] not in names and ':' not in s]
    extra += ['z8-serial5-config-model=on', 'engine153-alternate-bank-model=on', 'z8-jpeg-decode=on', 'wave5-upstream-model=on']
    prop_file = out.parent / (args.name + '-properties.json')
    prop_file.write_text(json.dumps(dict(model='Nikon Z8', main_sha256=sha_file(STOCK), properties=extra), indent=2) + '\n')

    run_args = SimpleNamespace(
        firmware=STOCK, qemu=args.qemu, raw=BASE / 'runs/raw-precapture-20261012/input14.raw',
        output=out, seconds=args.seconds, settle=1, half_seconds=2, capture_properties=prop_file,
        card=True, nvme_trace=False, gpio=True, rtc_status=True, tagged_input=True,
        capture_selector=True, capture_timing_input=True, hsfc_processing_input=False,
        menu_actions='', menu_only=True, wait_liveview=False, navigation_press_ms=400,
        navigation_release_ms=500, capture_pixels=False, wait_capture_work=False,
        skip_cseq_repeat=True, native_writer_observer=False,
    )
    try:
        probe.run(run_args)
    except Exception as ex:
        import traceback
        traceback.print_exc()
        print("QEMU_STOPPED:", repr(ex), flush=True)
    finally:
        out.mkdir(parents=True, exist_ok=True)
        # Clean up large card raw file to save disk
        for p in out.rglob('*.raw'):
            if p.stat().st_size > 1024 * 1024:
                p.unlink()
        summary = dict(
            passed=(ST['dram_verified'] and ST['lang_set'] and ST['phase'] == 'done'
                    and 'shot_01_fenestra_zh' in ST['shots']
                    and 'shot_02_live_photo_zh' in ST['shots']
                    and 'shot_03_live_pb_key_zh' in ST['shots']
                    and 'shot_04_wonderlantern_zh' in ST['shots']
                    and 'shot_05b_wlm_mod0_inner_zh' in ST['shots']
                    and 'shot_06b_wlm_mod1_inner_zh' in ST['shots']),
            phase=ST['phase'],
            dram_checks=ST['dram_checks'],
            portrait_gate=ST['portrait_gate'],
            font_slots_zh=ST['font_slots_zh'],
            shots=ST['shots'],
        )
        (out / 'v68-zh-summary.json').write_text(json.dumps(summary, indent=2) + '\n')
        print("SUMMARY", json.dumps(summary, indent=2), flush=True)


if __name__ == '__main__':
    main()
