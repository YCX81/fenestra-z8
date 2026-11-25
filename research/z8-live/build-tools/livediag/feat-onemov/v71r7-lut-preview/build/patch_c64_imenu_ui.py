#!/usr/bin/env python3
"""Stage 0 EL1 UI patch module fixing all 9 C64 i-menu, Live View OSD, Fn+dial & native icon defects.

Fixes (all new RX code & RODATA strictly placed in C64's own RX tail cave
0x40B65F18..0x40B70000; all relocated RW arrays placed in C64's own zeroed
DRAM 0xA11630000..0xA1163F000; 0 FP/SIMD; strict AAPCS64):

  1. 0x406A9274 (0x251D producer) & 0x406A94C4 (0x251E is_mono):
     - Extends C10..C64 (codes 40..94 = 0x28..0x5E) instead of clamping >0x27
       to old w22 (which froze 0x251D on C-7/SD).
     - Encodes C10..C64 into disjoint uint8_t range 0x40..0xE4 (Normal 0x40..0x76,
       Mono 0x77..0xAD, Flex 0xAE..0xE4), fitting snap_19b (strb) and driving
       LV OSD dirty detection (0x404319A4) on every transition.
  2. i-menu 12-grid Picture Control tile (BOTH paths):
     - Path 2A (Live View p=1496: 0x40443194 / 0x40441540 / 0x40446BD0):
       Relocates 40-entry 0xA0283B2E8 -> 95-entry 0xA11630000 and 40-entry
       0xA028B5EB8 -> 95-entry 0xA11630400 (all 4 refs) in C64 DRAM; appends
       all valid C10..C64 slots (including Flexible Color base=0x20) to LABELS,
       SELECTORS, NEW_MAP_95, NEW_CELLS_2A, and C64_STATE.
     - Path 2B (Control-Panel / Info 12-grid: 0x404DBCCC + 0xA028718C0):
       Extends 108-entry UI table (0x00..0x6B) to 383 entries (0x00..0x17E,
       mapping C10..C64 x 5 variants to 0x7E00..0x7F12) at 0x40B66000, updates
       the 3 descriptors (0x43A183F8, 0x43A18938, 0x43A18E60), and hooks
       0x404DBDB8 to decode 0x251D in 0x40..0xE4.
  3. i-menu dial quick strip (0x40453964, 0x4048EC18 & 0x404A5488):
     - Widens cell-reset bounds from cmp x8,#0x3c0 (40 cells) to cmp x8,#0x8e8
       (95 cells, safely within CELLS 0xC00 @ 0xA116C7400 and CELLS_ALT 0x1000
       @ 0xA116C8000), extends monochrome filter overlays for C10..C64, and
       expands dial carousel widget creation 0x404A579C/0x404A57D0 from 0x39
       (57) to 0x5F (95) primary widgets (165 / 1200 WidgetPool[0] slots) so
       indices 57..94 (C-27..C-64) render on the dial carousel bar.
  4. Live View OSD indicator hook LV_INDICATOR (0x40B5A000):
     - Removes broken `cbnz w19, old` and `cbnz w9, old` gates; reads active
       photo or video bank selector (0x403F9608 / 0x403F9650 respecting
       0x0300122C) and renders C10..C64 with exact 0..4 variant in both photo
       and video modes.
  5. state2 (p=1562) i-key / cancel exit overwriting active PC to AUTO (0):
     - 5A (0x40B52600 / 0x410AABC0): Supports rollback bank w1==2 (0x01001E28)
       for codes 40..94 instead of falling through to stock 0x410AAC90 (which
       returned 0=AUTO during 0x410D8400 rollback on i/half-shutter exit!).
     - 5B (0x404AD40C / 0x404ED30C / 0x404AC810): Seeds [0xA032F86D8+0x160]
       and params on p=1562 entry and guards 0x404AC810 via DRAM flag
       0xA11632000 so uninitialized state2 never writes AUTO(0).
  6. LABEL_CODE widget kind state machine (0x40B62000 & 0x40B62800):
     - SETTER (0x40B62000): Allows widget->kind == 0 -> 3 initialization
       (matching stock 0x40325D5C), preventing blank icons on DISP/EVF/LCD/menu
       reopen and fresh dial-strip cells; resets kind 1 -> 0 before 0x40325D48
       so both widget & parent dirty flags are set cleanly.
     - CONTENT_SETTER (0x40B62800): Handles kind 3 (owned 0x7E00..0x7F12) ->
       kind 1 (native bitmap) or kind 0 (hide/clear) by calling 0x4032B9EC to
       free [widget+0x68], resetting kind=0, and calling 0x40325D48.
  7. Custom Fn button ("Set Picture Control", hold mode 9) + Command Dial:
     - 7A (Appc Engine: 0x40D4E6CC / 0x40D39A20 / 0x40D491C8):
       * Replaces 40-entry Fn+dial PC table (0xA02822D48, native + C-1..C-9)
         with 95-entry table at DRAM_FN_PC_TABLE_95 (0xA11632100) including
         C-10..C-64 (codes 40..94, photo & video).
       * Replaces 0xA0 (160 B = 40*4) candidate buffer at 0x40D39A20 with
         0x180 (384 B >= 95*4) at DRAM_FN_CAND_BUF (0xA11632700), AND invokes
         stock publisher 0x410D951C once boot init completed ([0xA02948688+0x24]
         != 0) so async slotcard/cloud C-10..C-64 imports are republished into
         REGISTRY0/1 (0x07000633 / 0x0700122B), fixing both 0x41014828 rejection
         and stale 31-native-only candidate cache.
       * Refreshes 95-entry enabled flags & live index via 0x40D39964(0,0) on
         dial step at 0x40D491C8, masks slots > C64_LIMIT ([0xA116D3400]), and
         clamps cur_idx >= 95 (preventing the 0x7FFFFFFF OOB read/abort when
         stepping from C-10..C-64).
     - 7B (UI Fn-hold HUD p=1643 mode 9: 0x4049A41C..0x404A0100):
       * Relocates 40-entry 0xA02840848 -> 95-entry DRAM_HUD_MAP_95
         (0xA11632900, 3 refs) and widens 0x4049D324 lookup count 0x28 -> 0x5F.
       * Relocates 57-entry 0xA032F6038 -> 95-entry DRAM_HUD_CELLS_95
         (0xA11632C00, all 45 refs, 0 overflow into 0xA032F6590).
       * Appends/refreshes all valid C-10..C-64 slots (incl. Flexible Color
         base=0x20) in LABELS, SELECTORS, DRAM_HUD_MAP_95, NEW_CELLS_2A,
         DRAM_HUD_CELLS_95, and sub-dial monochrome filter index (0x4049D76C).
  8. Fresh-boot C-10..C-64 blank icons/text on LV Fn strip (p=1636/p=1643) & i-menu 12-grid (p=1496):
     - 8A (0x40B62444 -> hook_c64_style_cache):
       * Base C64 string lookup (0x40B62400 via 0x4032E70C) returns static
         0x7E00..0x7F12 labels with style_id=0x229 without invoking 0x403F2920,
         leaving lazy RAM font-style cache [0xA02F2AC30][0x229] (0xA032BF0C8)
         all zeros (font_w=0, font_h=0) on fresh boot until p=1562 opens.
       * Populates [0xA02F2AC30][style_id] via stock ROM descriptor 0x40695E3C
         and language-aware font resolver 0x403F29F4 (exact 7-field match to
         0x403F2984..0x403F29E8, also tracking live EN/CJK language switches).
     - 8B (superseded by Fix 9): 0x404D38C4 stays untouched stock so p=1496
       12-grid & Control Panel draw native kind=1 64x32 PNG icons directly
       from EXT_TILE_TABLE.
  9. Native Nikon Cloud/Custom kind=1 PNG icons (64x32 fg + 68x36 LV OSD outline)
     for C-10..C-64 across ALL UI surfaces (p=1636 Fn strip, p=1643 HUD, p=1496
     12-grid, i-menu dial strip, LV OSD viewfinder, Control Panel, Shooting Menus):
     - 9A/9B (0x4032DC80 -> hook_c64_bmp_desc, 0x4032DD60 -> hook_c64_bmp_ptr):
       * Hooks downstream of NFP v0.5.0 (0x40B1A000), Stars (0x40A41090), and
         Portrait (0x40963588) with zero conflict.
       * Synthesizes standard 8-bit grayscale (color_type=0, type=0x0100) PNGs
         on demand from 1:1 ROM-extracted Nikon Z8 4-bit frames (Normal 0x045A,
         Flexible Color [FC] 0x1824, Creative [C] 0x0891) + condensed C/-/0..9/*
         sprites into a 16-slot DRAM cache (0xA11634000..0xA1163F000, 44 KiB),
         matching stock C-1..C-9 64x32 geometry (0 clipping) and 68x36 2-pixel
         black drop-outline for LV OSD (0x7F20..0x8032).
     - 9C/9D (0x40B62000 -> hook_label_setter_native, 0x40B62800 ->
       hook_content_setter_native):
       * Stops forcing kind=1 icon widgets into kind=3 text, while preserving
         p=1562 Set Picture Control list kind=3 text ("C-10".."C-64") without
         the stock >0x12B clamp.
     - 9E (0x40B68A48 -> hook_lv_indicator_outline_tail):
       * Passes w3 = 0x7E00+idx (64x32 fg) and w4 = 0x7F20+idx (68x36 black
         outline) to 0x4041ECB4, matching stock C-1..C-9 2-layer LV OSD icons.

Interface:
  apply(image: bytearray) -> dict
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path
from typing import Dict, List, Tuple

BASE = 0x40160000
DATA_FILE_OFF = 0x4B13000
DATA_RAM_BASE = 0xA02808000

# Strictly owned RX tail cave inside C64 imenu_code (41,192 B verified zero)
CAVE_GUARD_START = 0x40B65F18
CAVE_START = 0x40B66000
CAVE_LIMIT = 0x40B70000

# Extended 383-entry 12-grid UI descriptor table (383 * 24 = 9,192 B = 0x23E8 B)
EXT_TILE_TABLE_VA = 0x40B66000
STOCK_TILE_TABLE_RAM = 0xA028718C0
STOCK_TILE_COUNT = 0x6C        # 108 entries (keys 0x00..0x6B)
C64_TILE_SLOTS = 55            # C-10..C-64
C64_TILE_VARIANTS = 5          # 5 variants per slot (0x7E00..0x7F12)
EXT_TILE_COUNT = STOCK_TILE_COUNT + C64_TILE_SLOTS * C64_TILE_VARIANTS  # 383 = 0x17F
EXT_TILE_ENTRY_SIZE = 24
EXT_TILE_BYTES = EXT_TILE_COUNT * EXT_TILE_ENTRY_SIZE  # 9,192 B

# Assembled AArch64 machine code placed at 0x40B68400 (0 FP/SIMD, clang-21 + ld.lld)
CODE_VA = 0x40B68400

# C64 DRAM scratch allocations inside verified-free 0xA1162B000..0xA1164D000
DRAM_NEW_MAP_95 = 0xA11630000       # 95 * 8 B  = 760 B (replaces 0xA0283B2E8)
DRAM_NEW_CELLS_2A = 0xA11630400     # 95 * 24 B = 2,280 B (replaces 0xA028B5EB8)
DRAM_S2_VALID = 0xA11632000         # 4 B u32 flag (p=1562 state2 adjust active)
DRAM_FN_PC_TABLE_95 = 0xA11632100   # 95 * 16 B = 1,520 B (replaces 0xA02822D48)
DRAM_FN_CAND_BUF = 0xA11632700      # 384 B (0x180 >= 95*4, replaces 0xA0 stack buf at 0x40D39A20)
DRAM_HUD_MAP_95 = 0xA11632900       # 95 * 8 B  = 760 B (replaces 0xA02840848 in p=1643)
DRAM_HUD_CELLS_95 = 0xA11632C00     # 95 * 24 B = 2,280 B (replaces 0xA032F6038 in p=1643)
DRAM_PNG_CACHE = 0xA11634000        # 16 * 0xB00 B = 45,056 B (64x32 fg + 68x36 LV outline PNG cache)

# Verified symbol entry points inside CODE_VA (0x40B68400..0x40B6A95C, 9,564 B)
SYMBOLS: Dict[str, int] = {
    'c64_variant':                 0X40B68400,
    'hook_251d_c64':               0X40B68460,
    'hook_251e_c64':               0X40B68504,
    'hook_44332c_init_map':        0X40B68520,
    'hook_4433d8_append_c64':      0X40B68558,
    'hook_443530_cells_c64':       0X40B68630,
    'hook_4416d0_refresh_labels':  0X40B686F4,
    'hook_404dbdb8_grid_getter':   0X40B68790,
    'hook_453a4c_dial_filt_a':     0X40B68840,
    'hook_48eccc_dial_filt_b':     0X40B688EC,
    'hook_lv_indicator_c64':       0X40B6899C,
    'hook_code_to_sel_c64':        0X40B68A6C,
    'hook_s2_enter':               0X40B68A98,
    'hook_s2_save_valid':          0X40B68AF4,
    'hook_404ac810_guard':         0X40B68B10,
    'hook_label_setter_c64':       0X40B68B34,
    'hook_content_setter_c64':     0X40B68BFC,
    'hook_fn_pc_table_95':         0X40B68C84,
    'hook_fn_pc_cand_buf':         0X40B68CEC,
    'hook_fn_pc_dial_guard':       0X40B68D24,
    'hook_49a544_init_hud_map':    0X40B68D98,
    'hook_49a5f4_append_c64':      0X40B68DD0,
    'hook_49a728_cells_c64':       0X40B68E74,
    'hook_49c92c_update_c64':      0X40B68F44,
    'hook_49d76c_subdial_idx':     0X40B68FA8,
    'hook_49e36c_refresh_labels':  0X40B69018,
    'hook_c64_style_cache':        0X40B690A8,
    'hook_4d38c4_tile_kind':       0X40B69124,
    'hook_c64_bmp_desc':           0X40B69150,
    'hook_c64_bmp_ptr':            0X40B691B0,
    'hook_label_setter_native':    0X40B69200,
    'hook_content_setter_native':  0X40B69240,
    'hook_lv_indicator_outline_tail': 0X40B6925C,
    'c64_get_or_build_png':        0X40B69280,
}

# Verified 9,564-byte AArch64 machine code + 4-bit native sprite RODATA payload
# (0x40B68400..0x40B69148 base Bugs 1-8 + 8 B align + 0x40B69150..0x40B6A95C Bug 9;
#  clang-21 -mgeneral-regs-only -Oz, 0 FP/SIMD, 0 writable sections)
CODE_PAYLOAD = bytes.fromhex(
    'f353bea9fe0b00f9133c0012f403012ae003132a1b45e2971f80007161000054'
    '200080520c0000147f022071c80000549f02007208008052690080520011891a'
    '050000149f02007248008052890080520011891afe0b40f9f353c2a8c0035fd6'
    '08a000511fd90071a8040054f763bfa9f703082a18008052bf060071c1000054'
    '804582520060a07267faec971f000071f8179f1ae82600110819180b091096d2'
    'c92ca2f24901c0f22929088b2a0140b95f050071610000543841427902000014'
    'f803142ae003182a9dfaec971f80007161000054e0ba021107000014e003182a'
    '98faec971f000072e8020111e9de01112011881af763c1a8df03ed17e003162a'
    'dd03ed1788a200511fd90071680000548dfaec970c04ed1788720051fb03ed17'
    'e80f00f9090080520a5b69f8ca5a29f8290500113f7d007183ffff54eb1f8052'
    '6a8109aaca5a29f8290500113f7d017183ffff54776be317f35fbda9f86b01a9'
    'fc7702a99f0376f2f8079f1a088086d2a82da2f24801c0f21a0140b95f030171'
    '080880521a819a1a530180527f021a6be803005408070011690600513731080b'
    'e003172a0d44e297e0020036e003172ae1a301915c44e29760020034fcf34279'
    'e003172a2846e297e103002ae0031c2a8cffff97682a0051a90080520801091b'
    '09c08f520801090b285b3578975a3bb8697a0011c84e358b09550029b5060011'
    '7b07001173060011e1ffff17083094d2882da2f24801c0f2186d0029fc7742a9'
    'f86b41a9f35fc3a813a789d26c6be317df020071f6079f1a088086d2a82da2f2'
    '4801c0f21c0140b99f030171080880521c819c1a590180523f031c6b88040054'
    'c8060011290700513531080be003152ada43e29780030036e003152ae1630691'
    '2944e29700030034e0534579f045e29760010036e00313aae103142ae2ef41b9'
    'e35306910f0ce2976b0180d26b00c0f2e89741b9ea031baa050000140b0080d2'
    '6b00c0f2080080520a0080d2095fba9b2b0100f9280900b93f3100392a0900f9'
    '1807001139070011dcffff17ffc30b91916be317df0276f2f8079f1a088086d2'
    'a82da2f24801c0f2190140b93f030171080880521981991a55018052bf02196b'
    '4803005408070011a90600513331080be003132aa943e29740020036e003132a'
    'e1230091f843e297c0010034fa334279e003132ac445e297e103002ae0031a2a'
    '28ffff97a82a0051a90080520801091b09c08f520801090b885a3778f7060011'
    'b5060011e6ffff17fd7b5ba9d263e317080001511f910271080500541fdd0071'
    'c30000541fb90171e300005413b90151340080521d000014f303082a15008052'
    '0300001413dd00513500805280318452f20ee297160080521f040071a1000054'
    '40a98452ed0ee2971f000071f6179f1ac8060011692600112031080b9a45e297'
    'd50000351f00007208008052690080521411891a050000141f00007248008052'
    '890080521411891aa80080527352081b73b2011154cde517e203002a60cde517'
    'ffa20071f7079f1a088086d2a82da2f24801c0f21b0140b97f03017108088052'
    '1b819b1a5c0180529f031b6bc8030054e8060011890700513331080be003132a'
    '5643e297c0020036e003132ae1230091a543e29740020034e03342796c45e297'
    'c0010036e00315aae103142ae25f40b9e3b305918b0be297485bb89b6b0180d2'
    '6b00c0f2e96f41b90b0100f9090900b91f310039190900f95a0700119c070011'
    'e2ffff17f37b5ba95aace317f35bbea9fb7301a9088086d2a82da2f24801c0f2'
    '1b0140b97f030171080880521b819b1a5c0180529f031b6ba803005489070051'
    '33054011e003132a2c43e297c0020036e003132ae1e300917b43e29740020034'
    'e09342794245e297c0010036e00315aae103142ae28f40b9e3730691610be297'
    '485fb89b6b0180d26b00c0f2e99f41b90b0100f9090900b91f310039190900f9'
    '5a0700119c070011e3ffff17fb7341a9f35bc2a814ec89d2ce98e417a8824039'
    'e8000034804582520060a0727b0ee297600000352743e297020000141343e297'
    '080440511ffd0071a80000541f250071c3040054090080520700001408084051'
    '1ffd0071280400541f250071e3030054290080520a19090b0b1096d2cb2ca2f2'
    '4b01c0f26b290a8b6c0140b99f050071c10200546c414279f763bfa917250051'
    'f8030c2a1045e297e103002ae003182a74feff97a800805204c08f52e412081b'
    '8400000bf763c1a805020014800480522100805202008052f37b42a9f55341a9'
    'f60743f894d8e2178fc5ff1708a000511fd90071e80000543f080071a8000054'
    '3f080071e103811a002900115daeff173f1000714c081514080084d2682ca2f2'
    '4801c0f21f0100b9803184523b0ee2971f040071e1000054804582520060a072'
    '360ee29760000035e242e29702000014ce42e29714db90d2f465a0f25401c0f2'
    '806201b9812200910f43e297143d8dd24812e517080084d2682ca2f24801c0f2'
    '29008052090100b915db90d20112e617080084d2682ca2f24801c0f2090140b9'
    '890000341f0100b9a81be6973a0fe517440fe51709c08f522800094b1f490471'
    'c9020054400500b4080840b90801094b1f490471c8040054080c40b91f0d0071'
    '61040054f353bea9fe0b00f9f30300aaf403012a9e0bdf977f0e00b9e00313aa'
    '21008052e203142a70f4de97fe0b40f9f353c2a8c0035fd6e00200b4080c40b9'
    'a80000341f050071600000541f0d007121020054f353bea9fe0b00f9f30300aa'
    'f403012a1f050071410000547f0e00b9e00313aa61008052e203142a5bf4de97'
    'fe0b40f9f353c2a8c0035fd6f40f1ef81c36e21700008052c0035fd609c08f52'
    '4800094b1f490471a80000543f04007161000054e103022ac7ffff17000300b4'
    '3f040071c8020054080c40b91f0d007161020054080840b90801094b1f490471'
    'e8010054f353bea9f57b01a9f30300aaf403012af503022a650bdf977f0e00b9'
    'e00313aae103142ae203152a37f4de97f57b41a9f353c2a8c0035fd65fac0471'
    '5c35e217600000b4e80b8052080000b9002084d2602ca2f24001c0f2080440b9'
    'e97b00321f01096b0002005401a985d24150a0f24101c0f2e30300aa02058052'
    '2a2cc1a86a2c81a842040071a1ffff540a0580526a248128697c81284a050011'
    '5f7d017183ffff54c0035fd608d190d28852a0f24801c0f20891403948000034'
    '07c2159408e084d2682ca2f24801c0f202308052e1830091e003162ae81b00f9'
    '45430714f70300aae17bbfa9e0031faae1031f2a0c430794e17bc1a8088086d2'
    'a82da2f24801c0f2080140b9280100341f010171e2000054087d0011e952288b'
    '3f310039080500111f7d017183ffff54380340b91f03016b63000054f8031f2a'
    '380300b9e00317aae203182ae303142a240080520f810714e80700f909008052'
    '8a5b69f8aa5b29f8290500113f7d007183ffff54eb1f80526a8109aaaa5b29f8'
    '290500113f7d017183ffff54dfc5e417088086d2a82da2f24801c0f21a0140b9'
    '5f030171080880521a819a1a530180527f021a6bc80300546806005114054011'
    'e003142af541e297e0020036e003142ae1a300914442e29760020034f9734279'
    'e003142a1044e297e103002ae003192a74fdff97682a0051a90080520801091b'
    '09c08f520801090ba85a3778745b38b8697a0011a84f378b095d0029f7060011'
    '1807001173060011e2ffff170a8e81d2e2c5e417088086d2a82da2f24801c0f2'
    '170140b9ff020171080880521781971af77e0011f7f27dd3182880d21f0317eb'
    'e2040054d56a78b8e003152acb41e29720040036e003152ae10306911a42e297'
    'a0030034e0234579e143e29760010036e00313aae103142ae2d741b9e3730b91'
    '000ae297e8df42b9e9031daa6a0180d26a00c0f205000014e8031f2ae9031faa'
    '0a0080d26a00c0f22b6fbc9b2c6bbc9b6a0100f9680900b97f310039690900f9'
    '8a0100f9880900b99f310039890900f93907001118230091d9ffff1745d0e497'
    'fbc5e417088086d2a82da2f24801c0f21c0140b99f030171080880521c819c1a'
    '9c7f00119cf37dd3142880d29f021ceb82010054a06a74b89841e297e0000036'
    '20008052e103132a63d1e497685ab79b73060011000900b994220091f4ffff17'
    '9ad4e49763cee4177f7e007123030054ff6f01b909408ed2892da2f24901c0f2'
    '687e00511f01017102020054205968b8e1230091d441e29780010034e0334279'
    '9b43e29720010036003c94d22074a8f261018052e25f40b9e3b30591b909e297'
    'e06f41b985d2e417e0031f2a83d2e417090093d2d7d1e417088086d2a82da2f2'
    '4801c0f2180140b91f030171080880521881981a530180527f02186b28030054'
    '6806005114054011e003142a6341e29740020036e003142ae1230091b241e297'
    'c0010034f6334279e003142a7e43e297e103002ae003162ae2fcff97682a0051'
    'a90080520801091b09c08f520801090ba85a3778f706001173060011e7ffff17'
    'fd7b5ba9b3d4e4176a0000b9088695d2485ea0f24801c0f2080140f9280300b4'
    'f353bea9f57b01a9f3030a2a1451338be003132a5ab3ec97f50300aae003132a'
    '4526e297a9064039aa0e4039ab0a4039ac124039ad0a40b9ae32403980020039'
    '890600398a0a00398b0e00398c1200398d0a00b98e320039f57b41a9f353c2a8'
    'c0035fd6690a00b90ac08f522a010a4b5f490471880000541f05007141000054'
    '68008052e1a9e517'
    # 8-byte align (0x40B69148..0x40B69150) + Bug 9 native kind=1 PNG synthesizer & 4-bit sprites (0x40B69150..0x40B6A95C, 6,156 B)
    '00000000000000000ac08f520b010a4b7fc90871280200547f49047169000054'
    '7f810471a3010054f37bbea9f30301aae003082ae14300914000009408208052'
    'e91340b968020079690600b9600a00f9f37bc2a8c0035fd61f01096b4a000054'
    'b912df17b612df170bc08f520c000b4b9fc90871c80100549f49047169000054'
    '9f81047143010054f353bea9fe0b00f9f30301aaf40302aa28000094800200f9'
    'fe0b40f9f353c2a8c0035fd61f01006b290140f9da12df1709c08f522800094b'
    '1fc9087128010054400100b4f37bbfa9e203012a010c40b9caf2de9700000012'
    'f37bc1a8c0035fd6f40f1ef88b34e21700008052c0035fd609c08f524800094b'
    '1fc9087148000054bef2de175fac0471e633e217e303042a8480041180048052'
    '2100805202008052f37b42a9f55341a9f60743f88ed6e217fd7bbaa9fc6f01a9'
    'fd030091fa6702a9f85f03a9f65704a9f44f05a9ff4320d1197c0553e8e38f12'
    'e9bf8f120b0088d20a6081520c0288d23fe70f716b2ca2f26c2ca2f22831881a'
    '090180524b01c0f20801000be933891a4c01c0f21f490471f50301aae883881a'
    '09090033382daa9b3431aa9b0b0340b97f01006b61000054080740b92d010014'
    'f303002ae9031faaea2300913f0120f1800000545f69293829050091fcffff17'
    '093d0012aa008052e02300912909ca1ac2038052c303805204048052e5031f2a'
    'e6031f2af62300912a09090b3a290011093c805217010a4b08788052ff120071'
    'e41a427a08019f9aff0600712801889a090000b029f10e912101088b2b010094'
    '480180525b0bc81a280180127a6b081be80e00511f090071e202005401000090'
    '21f83a91e023009102018052e300805284038052c503805241010094c8960f91'
    'e9018052ea1f80523f4900f100040054eb031faa7f0d00f1800000540a692b38'
    '6b050091fcffff172905009108010191f6ffff170100009021302391e0230091'
    '020180520301805284038052e50380522b010094c89e0f91e9018052ea1f8052'
    '3f4900f180050054eb031faa7f1100f1800000540a692b386b050091fcffff17'
    '2905009108010191f6ffff17686319530900009029b93c9136497b3837497a38'
    'e023009108111b4b1b0000907be33c9102018052e303162a840380526103088b'
    '250580520e01009448631953e0230091c5aa001102018052e303172a84038052'
    '08111a4b460080526103088bdf000094010000b021600e91e0230091c2008052'
    'c30080528401805245078052e6031f2a180000149c1180520800009008f12491'
    '697f1c1b16497b381b0000907b1b259117497a38e023009142018052e303162a'
    '840380526103098b85058052ec000094487f1c1be0230091c5b6001142018052'
    'e303172a84038052460080526103088bbe00009428118ad209a0a1d23fe70f71'
    'c8e9a8f22909c9f20a048052a841c1f28948eaf2007300914843e1f221028052'
    '1f430079082701a988088052090880522831881a890480521f8b0039088f0039'
    '5731891a08010032167d171b080180521f4b00791f9b0039179f0039082b00b9'
    'da2e00111fb300398c0000942b898852e803002ae00318aa2b88aa720c7d1853'
    '0d7d08530b5c03b80b7d10530c801f38ec03362ae9031faa0b901f38cb7e0853'
    'ea031f2a08b01f38487f0853ee2300910b2000398b7d085308e01f38082f8052'
    '0da01f380dfe9f521af01f381a2c009108080079280080520c2400390cfe9f12'
    '0b280039eb2300911fc01f7808180039161c00393f0117eb800800544a01080b'
    '5f1700384f010c0b5f010d6bea818a1a3fe70f7162020054ef030eaa10088052'
    'b00100b4f1154038100600d10801110b5117003812010c0b1f010d6b4882881a'
    '0a010a0b51010c0b5f010d6b2a828a1ab0feffb529050091ce010191e6ffff17'
    'ef031f2a30090051ff11017140ffff54f1031f2a320080125f0e007100030054'
    '0202120b5f7c00716900005452060011faffff17417e121b6219028b23008012'
    '7f0c007140ffff546404031b9f14007128010054e401030b840800519ffc0071'
    'a800005444486438251e00129f00316b9180851a63040011f2ffff170801310b'
    '51170038ef05001112010c0b1f010d6b4882881a0a010a0b51010c0b5f010d6b'
    '2a828a1ad9ffff1709410a2a0a7d0853c13e0011480f00392b7d1853297d1053'
    '4a0b00394b030039490700391b000094097c185328a9c8d20a7c0853c889e8f2'
    '401f003949130039097c1053480700f9c855885249170039084cb0724903144b'
    '481300b9285100114a1b003913230029a80200b9e00314aaff432091f44f45a9'
    'f65744a9f85f43a9fa6742a9fc6f41a9fd7bc6a8c0035fd60b649052e8031faa'
    'e903012a0a0080120bb7bd721f0109eb800100540c6868384a010c4a0c018052'
    'cc0000344d0100138c050051ad010b0aaa054a4a8cffff3508050091f4ffff17'
    'e0032a2ac0035fd6e903062a4a7c0153e8031faa0b18098bec03052aed03042a'
    'ee03032a1f010deb800300540f01098bff7d00f189000054080500916b010191'
    'f9ffff1710050a9bef031faa71010c8bdf010feb20ffff5492010f8b5ffe00f1'
    '88010054f2fd41d3ff0140f2126a7238407e0453520e00125202801a206a6f38'
    '5212122a5f02006b49000054326a2f38ef050091efffff17c0035fd646008052'
    'daffff1700c3ff7f40ffffffd1fffffff4ff18d4faaf0000fd6f0000ff3e0000'
    'ff2d0000ff0d0000ff0d0000ff0c0000ff0c0000ff0c0000ff0c0000ff0c0000'
    'ff0c0000ff0c0000ff0c0000ff0c0000ff0d0000ff1d0000ff2e0000fc5f0000'
    'f89f0000f3ff17e4c1ffffff30ffffff00c3ff7f0907090909090909090900e5'
    'ef050050ffff5f00c2ffffcf02f4bfb2ff04f84f40ff08fb3d30fd0bfe1b10fb'
    '0eff1a10fa0fff0900f90fff0800f80fff0800f80fff0800f80fff0800f80fff'
    '0800f80fff0800f80fff0800f80fff0800f80fff0800f80fff0800f80fff0900'
    'f90fff1a10fa0ffd2c20fc0dfa3e30fe0af65f50ff06f3dfd3ff03a1ffffaf01'
    '40ffff4f0000d4df04000000b10f000060ff0f0010fbff0f00f6ffff0f00feff'
    'ff0f00f86fff0f00b301ff0f000000ff0f000000ff0f000000ff0f000000ff0f'
    '000000ff0f000000ff0f000000ff0f000000ff0f000000ff0f000000ff0f0000'
    '00ff0f000000ff0f000000ff0f000000ff0f000000ff0f000000ff0f000000ff'
    '0f000000ff0f000000ff0f000000ff0f000000ff0f0020faff0800f2ffffbf00'
    'fdffffff05ff2dd2ff0ccf0030ff0f1e0000ff0f040000ff0f000010ff0f0000'
    '30ff0d000050ff0a000090ff060000c0ff020000f0ff000000f3af000000f85f'
    '000000fe0f000020ff0c000080ff060000f0ff010000f5bf000000fd4f000030'
    'ff0f0000c0ff090000f3ff020000fdcf000000ffffffff0fffffffff0fffffff'
    'ff0f20fa9f0200c1ffff1a00f2ffff5f00f22af7bf01c201b1ff014000b0ff02'
    '0000b0ff020000b0ff020000d3ff010000f7bf010082ff5f0040ffff180040ff'
    'ff170040ffff6f000010e6ff01000070ff07000020fe0c000000fd0f000000fc'
    '0f000000fc0f000000fd0f000020fd0f050040ff0e3f0090ff08ff15f7ff02ff'
    'ffff8f00f8ffff2b0050fe8f0100000020fd02000060ff020000a0ff020000d3'
    'ff020000f6ff020000f9ff020030fcff020050ffff020081ffff0200c3ffff02'
    '00f5ffff0210f8ffff0220fdedff0240ffb9ff0281ffb6ff02e1dfb3ff02f3af'
    'b0ff02f96fb0ff02ff2db0ff02ffffffff0fffffffff0fffffffff0f0000b0ff'
    '020000b0ff020000b0ff020000b0ff020000b0ff020000b0ff02f2ffffff02f2'
    'ffffff02f2ffffff02f2bf000000f2bf000000f2bf000000f2bf000000f2bf00'
    '0000f2bf000000f2bf000000f2ff4c0000f2ffff0700f2ffff4f00a122fbcf01'
    '0000c2ff04000070ff09000040fe0d000010fd0f000000fd0f000000fd0f0000'
    '10fd0f000040fe0e000070ff090600c2ff039f23fbcf01ffffff4f00ffffff17'
    '00b2ff4c00000080ff7f0000fcffff0c80ffffff0ff1ff1a920ff6df000006fb'
    '6f000000fe2f000000ff0f000000ff0f000000ff0f000000ff8fff0800ffffff'
    '8f00ffffffff01ffffffff08ffdfd1ff0dff4f40ff0fff0f10ff0fff0f00ff0f'
    'ff0f00ff0fff0f00ff0fff0f00ff0fff0f00ff0fff0f10ff0fff4f40ff0ffadf'
    'd1ff0af3ffffff0380ffff8f0000f6ff0600ffffffff0fffffffff0fffffffff'
    '0f000081ff0f0000a3ff0c0000c4ff090000e6ff050000f8ff020010faef0000'
    '30fbbf000060fd8f000080ff5f0000a0ff2e0000c1ff0c0000d4ff090000e7ef'
    '070000f9df040000fccf010020feaf000050ff8f000080ff6d0000b0ff3c0000'
    'e0ff1a0000f2ff080000f5ef060000f9cf040000fcaf030000ff8f01000000d4'
    'df040030ffff3f00a1ffffaf01f1cfc4ff01f2bfb0ff02f2bfb0ff02f2bfb0ff'
    '02f2bfb0ff02f2bfb0ff02f1bfb0ff01a1cfc4af0130ffff3f0010f8ff180080'
    'ffff8f00f3ffffff03fafffbff0aff4e40fe0fff0d00fd0fff0c00fc0fff0c00'
    'fc0fff0c00fc0fff0c00fc0fff0c00fc0ffd4e40fe0df8fffbff08f2ffffff02'
    '60ffff6f0000d5df050010faff1a00c0ffffcf00f3ffffff03f9ffffff08fccf'
    'c1ff0cfe5f60ff0eff2f20ff0fff1f10ff0fff0f00ff0fff0f00ff0fff1f10ff'
    '0fff2f20ff0fff3f30ff0ffd6f60ff0ffadfd1ff0ff5ffffff0fe0ffffff0f20'
    'fbcfff0f000000ff0f000000ff0f000000ff0f000000ff0f000030ff0f060090'
    'ff0daf02f6ff07ffffffff01ffffff8f00fbffff0c0000f7ff0970ffff0ff3ff'
    'ff0ff6ef320dfb3f0000ff1e0000ff1b0000ff090000ff070000ff070000ff07'
    '0000ff070000ff070000ff070000ff070000ff070000ff070000ff070000ff07'
    '0000ff070000ff080000ff090000fe1d0000fa2f0000f5df320df2ffff0f70ff'
    'ff0f00f6ff090806080808080808080800f99f0080ffff08f3ffff3ff56ff65f'
    'f90ff0affd0bb0dfff0880ffff0650ffff0440ffff0440ffff0330ffff0330ff'
    'ff0330ffff0330ffff0330ffff0330ffff0330ffff0330ffff0440ffff0550ff'
    'ff0660fffe0990effc0cc0cff81ff18ff48ff84fd2ffff2d60ffff0600f88f00'
    '0010f90000c2ff0040feff00f7ffff00ffffff00fadcff0094a1ff0000a0ff00'
    '00a0ff0000a0ff0000a0ff0000a0ff0000a0ff0000a0ff0000a0ff0000a0ff00'
    '00a0ff0000a0ff0000a0ff0000a0ff0000a0ff0000a0ff0000a0ff0000a0ff00'
    '00a0ff0000a0ff0000a0ff0000a0ff0040fdbf02f4ffff1efeffff7fff2af8df'
    'af00e2ff0e00d0ff0400d0ff0000d1ff0000d2ff0000e3bf0000f58f0000f74f'
    '0000f92f0010fc1d0040fe080060ff040091ef0300d3af0100f56f0010fa4f00'
    '30ff1b0070ff0700e1ff0400f5bf0100fe7f0000ffffffffffffffffffffffff'
    '40fd3d00f2ffef03f4ffff08f427fe2da300f53f3100f44f0000f44f0000f54f'
    '0000f83f0000ff2e00d3ff0880ffcf0280ffbf0280ffff090010fc3f0000f28f'
    '0000a1df000080ff000080ff000080ff000080ff0000a1ff0500c2ff2f00f3af'
    'ef24fe4fffffff1bfaffff0370ff3c000000a14f0000e24f0000f44f0000f84f'
    '0000fe4f0010ff4f0060ff4f00b0ff4f00f2ff4f00f6ff4f00faff4f20fdff4f'
    '50fffa4f70fff54fb1eff44ff38ff44ff54ff44ffb2ef44fff1af44fffffffff'
    'ffffffffffffffff0000f44f0000f44f0000f44f0000f44f0000f44f0000f44f'
    'f4ffff4ff4ffff4ff4ffff4ff44f0000f44f0000f44f0000f44f0000f44f0000'
    'f44f0000f44f0000f4ff1800f4ffbf01f4ffff079242ff2f0000f75f0000f3bf'
    '0000b1ef000090ff000080ff000080ff000090ff0000b1ff0000f3bf0600f75f'
    '8f42ff2effffff08ffffbf01d3ff180000c3ff1a30ffffdfb1fffffff3ff25f7'
    'f87f0060fc3e0000ff1d0000ff0d0000ff0c0000ff0c0000ffcfbf02ffffff1b'
    'ffffff3fffffffafff8ff8efff2ee2ffff0dd0ffff0cc0ffff0cc0ffff0cc0ff'
    'ff0cc0ffff0cc0ffff0dd0ffff2ee2fffc8ff8cff5ffff5fb1ffff1b10f99f01'
    'ffffffffffffffffffffffff0000f2ff0000f6df0000f99f0000fd6f0000ff2f'
    '0020ff0f0060ff0d0090ff0900d0ff0600f0ff0200f2ff0000f6df0000f99f00'
    '00fd6f0000ff2f0020ff0f0060ff0d0090ff0900d0ff0600f0ff0200f2ff0000'
    'f6df0000f99f0000fd6f0000ff2f000000f77f0060ffff06d2ffff2df37ff73f'
    'f44ff44ff44ff44ff44ff44ff44ff44ff44ff44ff34ff43fc27ff72c60ffff06'
    '20fddf02b1ffff1bf5ffff5ffcdffdbfff1cc1ffff0880ffff0880ffff0880ff'
    'ff0880ffff0880ffff0880fffe1cc1eff9dffd9ff3ffff3f91ffff1910f88f01'
    '30fddf03e1ffff1ef5ffff5ffaffffaffd8ff8dfff3ee3ffff1dd1ffff0dd0ff'
    'ff0dd0ffff0dd0ffff0dd0ffff1dd1ffff2ee2ffff3ff3fffc8ff8fff7ffffff'
    'f2ffffff40feffff0000c0ff0000c0ff0000c0ff0000d0ff0000d2ff0600f5ef'
    '9f32fd9fffffff3fffffff1bfdffef0300ff0000ff00affffafffffff7ff7fd1'
    'ff1d60ff06d1ff1df7ff7fffffffaffffa00ff00000000000000000000000000'
    '0000000000000000000000000000000000000000000000000000000000000000'
    '00ffffffffffffffffffffffffffff0fffffffffffffffffffffffffffff0fff'
    'ffffffffffffffffffffffffff0fff0f0000000000000000000000ff0fff0f00'
    '00000500000000000000ff0fff0f0000100e0060ff9f000000ff0fff0f0000a0'
    '0f00f3ff0c000000ff0fff0f0000f50f10fdef02000000ff0fff0f0010fe0fc0'
    'ff3f00b70400ff0fff0f00a0ff0bf9ff0670ffbf04ff0fff0f00a0ff04000000'
    'f7ffff08ff0fff0f0010be00000000b4ff8f00ff0fff0f0000450000000000b4'
    '0800ff0fff0f0000000000000000000000ff0fff0fa001000000000000000000'
    'ff0fff0ff05e00000000000000ffffff0fff0ff0ff000000000000005d00008a'
    'ff0ff0ff00000000000000e30100e1ff0f50fe00000000000000800a0050ff0f'
    '00a10000000000000000fdffffff0f0000000000000000000000ff0fff0f0000'
    '000000000000000000ff0fff0f0000000000000000000000ff0fffffffffffff'
    'ffffffffffffffff0fffffffffffffffffffffffffffff0fffffffffffffffff'
    'ffffffffffff0f00000000000000000000000000000000000000000000000000'
    '0000000000000000000000000000000000000000000000000000000000000000'
    '0000000000000000000000000000000000000000000000000000000000000000'
    '00ffffffffffffffffffffffffffff0fffffffffffffffffffffffffffff0fff'
    'ffffffffffffffffffffffffff0fff0f0000000000000000000000ff0fff0f00'
    '00000500000000000000ff0fff0f0000100e0060ff9f000000ff0fff0f0000a0'
    '0f00f3ff0c000000ff0fff0f0000f50f10fdef02000000ff0fff0f0010fe0fc0'
    'ff3f00b70400ff0fff0f00a0ff0bf9ff0670ffbf04ff0fff0f00a0ff04000000'
    'f7ffff08ff0fff0f0010be00000000b4ff8f00ff0fff0f0000450000000000b4'
    '0800ff0fff0f00000000000000000000000000ff0fa001000000000000000000'
    '0000ff0ff05e000000f0ffffff0070ed2bff0ff0ff000000f0ffffff00faffef'
    'ff0ff0ff000000f00f000040ef04a3ff0f50fe000000f00f0000a06f0000ff0f'
    '00a1000000f00f0000d02f0000ff0f0000000000f0ffff0ff00f0000ff0f0000'
    '000000f0ffff0ff00f0000ff0f0000000000f00f0000d02f0000ffffffffffff'
    '0ff00f0000a06f0000ffffffffffff0ff00f000040ef04a3ffffffffffff0ff0'
    '0f000000faffef00000000000000f00f00000070fd2b00000000000000000000'
    '0000000000000000000000000000000000000000000000000000000000000000'
    '0000000000000000000000000000000000000000000000000000000000000000'
    '00ffffffffffffffffffffffffffff0fffffffffffffffffffffffffffff0fff'
    'ffffffffffffffffffffffffff0fff0f0000000000000000000000ff0fff0f00'
    '00000500000000000000ff0fff0f0000100e0060ff9f000000ff0fff0f0000a0'
    '0f00f3ff0c000000ff0fff0f0000f50f10fdef02000000ff0fff0f0010fe0fc0'
    'ff3f00b70400ff0fff0f00a0ff0bf9ff0670ff7f00fa0fff0f00a0ff04000000'
    'f7ff0a00500eff0f0010be00000000b4bf00200001ff0f000045000000000013'
    '70fe4e00ff0f000000000000000000fbffaf00ff0fa001000000000000a0cf14'
    '9b00ff0ff05e000000000000f61d000100ff0ff0ff000000000000fe05000000'
    'ff0ff0ff000000000060ef01000000ff0f50fe0000000000b0bf00000000ff0f'
    '00a10000000000f09f00000000ff0f00000000000000f29f00000020ff0f0000'
    '0000000000f3df000000d2ff0f00000000000000f3ff060050eeffffffffffff'
    'ffff03e0ffaf76fc5fffffffffffffffff0960ffffffff07ffffffffffffffff'
    '1e00f9ffff5e000000000000000000000060fd8d020000000000000000000000'
    '0000000000000000000000000000000000000000'
)


def _encode_b(src_va: int, dst_va: int) -> bytes:
    delta = dst_va - src_va
    if delta % 4 != 0 or not (-(1 << 27) <= delta < (1 << 27)):
        raise ValueError(f'branch out of range: {src_va:#x} -> {dst_va:#x}')
    return struct.pack('<I', 0x14000000 | ((delta >> 2) & 0x03FFFFFF))


def _encode_movz(reg: int, imm16: int, shift: int = 0) -> bytes:
    hw = shift // 16
    return struct.pack('<I', 0xD2800000 | (hw << 21) | ((imm16 & 0xFFFF) << 5) | (reg & 31))


def _encode_movk(reg: int, imm16: int, shift: int) -> bytes:
    hw = shift // 16
    return struct.pack('<I', 0xF2800000 | (hw << 21) | ((imm16 & 0xFFFF) << 5) | (reg & 31))


def _sha256(buf: bytes | bytearray) -> str:
    return hashlib.sha256(buf).hexdigest()


# Exact pre-patch byte assertions & replacements for all branch/instruction/descriptor sites
BRANCH_HOOKS: List[Tuple[str, int, str, str]] = [
    # (name, hook_va, expected_old_hex, target_symbol_name)
    ('bug1a_251d_producer',   0x406A930C, 'e003162a', 'hook_251d_c64'),
    ('bug1b_251e_is_mono',    0x406A9504, '88720051', 'hook_251e_c64'),
    ('bug2a_init_map95',      0x4044332C, 'e80f00f9', 'hook_44332c_init_map'),
    ('bug2a_append_c64',      0x404433D8, '13a789d2', 'hook_4433d8_append_c64'),
    ('bug2a_cells_c64',       0x40443530, 'ffc30b91', 'hook_443530_cells_c64'),
    ('bug2a_refresh_labels',  0x404416D0, 'fd7b5ba9', 'hook_4416d0_refresh_labels'),
    ('bug2b_12grid_getter',   0x404DBDB8, 'e203002a', 'hook_404dbdb8_grid_getter'),
    ('bug3a_dial_filt_a',     0x40453A4C, 'f37b5ba9', 'hook_453a4c_dial_filt_a'),
    ('bug3b_dial_filt_b',     0x4048ECCC, '14ec89d2', 'hook_48eccc_dial_filt_b'),
    ('bug4_lv_indicator',     0x40B5A000, '33050035', 'hook_lv_indicator_c64'),
    ('bug5a_rollback_bank2',  0x40B52600, '08a00051', 'hook_code_to_sel_c64'),
    ('bug5b_s2_enter_seed',   0x404AD40C, '143d8dd2', 'hook_s2_enter'),
    ('bug5b_s2_save_valid',   0x404ED30C, '15db90d2', 'hook_s2_save_valid'),
    ('bug5b_ac810_guard',     0x404AC810, '6e0c0194', 'hook_404ac810_guard'),
    ('bug6a_label_setter',       0x40B62000, '09c08f52', 'hook_label_setter_native'),
    ('bug6b_content_setter',     0x40B62800, '09c08f52', 'hook_content_setter_native'),
    ('bug7a_fn_pc_table_95',     0x40D4E6CC, '600000b4', 'hook_fn_pc_table_95'),
    ('bug7a_fn_pc_cand_buf',     0x40D39A20, 'e8c30191', 'hook_fn_pc_cand_buf'),
    ('bug7a_fn_pc_dial_guard',   0x40D491C8, 'f70300aa', 'hook_fn_pc_dial_guard'),
    ('bug7b_hud_init_map',       0x4049A544, 'e80700f9', 'hook_49a544_init_hud_map'),
    ('bug7b_hud_append_c64',     0x4049A5F4, '0a8e81d2', 'hook_49a5f4_append_c64'),
    ('bug7b_hud_cells_c64',      0x4049A728, '4a0a0094', 'hook_49a728_cells_c64'),
    ('bug7b_hud_update_c64',     0x4049C92C, '37060094', 'hook_49c92c_update_c64'),
    ('bug7b_hud_subdial_idx',    0x4049D76C, '090093d2', 'hook_49d76c_subdial_idx'),
    ('bug7b_hud_refresh_labels', 0x4049E36C, 'fd7b5ba9', 'hook_49e36c_refresh_labels'),
    ('bug8a_c64_style_cache',    0x40B62444, '6a0000b9', 'hook_c64_style_cache'),
    ('bug9a_bmp_desc_c64',       0x4032DC80, '6b000054', 'hook_c64_bmp_desc'),
    ('bug9b_bmp_ptr_c64',        0x4032DD60, '290140f9', 'hook_c64_bmp_ptr'),
]

# Exact AArch64 encoding of `cmp x8, #0x8e8` (SUBS XZR, X8, #0x8e8 -> 0xF123A11F)
CMP_X8_8E8 = struct.pack('<I', 0xF1000000 | (0x8E8 << 10) | (8 << 5) | 31)
assert CMP_X8_8E8 == bytes.fromhex('1fa123f1')
# Exact AArch64 encodings for dial carousel 57 -> 95 primary widget expansion (0x404A5488)
CMP_W21_5F = struct.pack('<I', 0x71000000 | (0x5F << 10) | (21 << 5) | 31)   # cmp w21, #0x5f (95)
assert CMP_W21_5F == bytes.fromhex('bf7e0171')
ADD_W21_W24_6C = struct.pack('<I', 0x11000000 | (0x6C << 10) | (24 << 5) | 21)  # add w21, w24, #0x6c (0x0d+95)
assert ADD_W21_W24_6C == bytes.fromhex('15b30111')
# Exact AArch64 encoding of `mov w20, #0x5f` (95 entries for DRAM_HUD_MAP_95 lookup at 0x4049D324)
MOV_W20_5F = struct.pack('<I', 0x52800000 | (0x5F << 5) | 20)
assert MOV_W20_5F == bytes.fromhex('f40b8052')

# Direct fixed-byte edits (dial strip cmp/widget bounds, HUD map count, & 3 UI table descriptors)
DIRECT_EDITS: List[Tuple[str, int, bytes, bytes]] = [
    # Bug 3: cmp x8, #0x3c0 (40 cells) -> cmp x8, #0x8e8 (95 cells = 2,280 B)
    ('bug3a_cmp_cells_95',    0x40453964, bytes.fromhex('1f010ff1'), CMP_X8_8E8),
    ('bug3b_cmp_alt_95',      0x4048EC18, bytes.fromhex('1f010ff1'), CMP_X8_8E8),
    # Bug 3c: dial carousel widget pool 0x404A5488: 0x39 (57) -> 0x5F (95) primary widgets (0x1100..0x115E)
    # WidgetPool[0] cap is 1200; 13 + 95 + 57 = 165 widgets, secondary row shifts 0x46 -> 0x6C
    ('bug3c_carousel_cnt_95', 0x404A579C, bytes.fromhex('bfe60071'), CMP_W21_5F),
    ('bug3c_carousel_off_6c', 0x404A57D0, bytes.fromhex('151b0111'), ADD_W21_W24_6C),
    # Bug 7b: p=1643 mode 9 get_cur_idx map search count: mov w20, #0x28 (40) -> mov w20, #0x5f (95)
    ('bug7b_hud_map_cnt_95',  0x4049D324, bytes.fromhex('14058052'), MOV_W20_5F),
    # Bug 2B: 3 descriptors pointing to 0xA028718C0 (count 0x6C) -> 0x40B66000 (count 0x17F)
    ('bug2b_desc_photo_0x10', 0x43A183F8,
     struct.pack('<QI', STOCK_TILE_TABLE_RAM, STOCK_TILE_COUNT),
     struct.pack('<QI', EXT_TILE_TABLE_VA, EXT_TILE_COUNT)),
    ('bug2b_desc_video_0x5f', 0x43A18938,
     struct.pack('<QI', STOCK_TILE_TABLE_RAM, STOCK_TILE_COUNT),
     struct.pack('<QI', EXT_TILE_TABLE_VA, EXT_TILE_COUNT)),
    ('bug2b_desc_alt_0xbd',   0x43A18E60,
     struct.pack('<QI', STOCK_TILE_TABLE_RAM, STOCK_TILE_COUNT),
     struct.pack('<QI', EXT_TILE_TABLE_VA, EXT_TILE_COUNT)),
]

# Audited 64-bit MOVZ/MOVK pointer relocations (exact instruction positions & old words)
POINTER_RELOCS: List[Tuple[str, int, int, int, List[Tuple[int, int, int]]]] = [
    # (name, reg, old_va, new_va, [(insn_va, shift_bits, expected_u32), ...])
    ('reloc_map95_4432a8', 22, 0xA0283B2E8, DRAM_NEW_MAP_95, [
        (0x404432A8,  0, 0xD2965D16),
        (0x404432E0, 16, 0xF2A05076),
        (0x404432FC, 32, 0xF2C00156),
        (0x40443320, 48, 0xF2E00016),
    ]),
    ('reloc_map95_446bd0', 25, 0xA0283B2E8, DRAM_NEW_MAP_95, [
        (0x40446BD0,  0, 0xD2965D19),
        (0x40446BD4, 16, 0xF2A05079),
        (0x40446BD8, 32, 0xF2C00159),
        (0x40446BDC, 48, 0xF2E00019),
    ]),
    ('reloc_cells2a_44340c', 23, 0xA028B5EB8, DRAM_NEW_CELLS_2A, [
        (0x4044340C,  0, 0xD28BD717),
        (0x40443414, 16, 0xF2A05177),
        (0x4044341C, 32, 0xF2C00157),
        (0x40443428, 48, 0xF2E00017),
    ]),
    ('reloc_cells2a_43fd64', 23, 0xA028B5EB8, DRAM_NEW_CELLS_2A, [
        (0x4043FD64,  0, 0xD28BD717),
        (0x4043FD6C, 16, 0xF2A05177),
        (0x4043FD74, 32, 0xF2C00157),
        (0x4043FD84, 48, 0xF2E00017),
    ]),
    ('reloc_cells2a_43fdd8', 23, 0xA028B5EB8, DRAM_NEW_CELLS_2A, [
        (0x4043FDD8,  0, 0xD28BD717),
        (0x4043FDE0, 16, 0xF2A05177),
        (0x4043FDE8, 32, 0xF2C00157),
        (0x4043FDFC, 48, 0xF2E00017),
    ]),
    ('reloc_cells2a_49a624', 27, 0xA028B5EB8, DRAM_NEW_CELLS_2A, [
        (0x4049A624,  0, 0xD28BD71B),
        (0x4049A63C, 16, 0xF2A0517B),
        (0x4049A650, 32, 0xF2C0015B),
        (0x4049A670, 48, 0xF2E0001B),
    ]),
    # Bug 7b: p=1643 mode 9 HUD map (0xA02840848, 40*8B -> DRAM_HUD_MAP_95, 95*8B)
    ('reloc_hudmap95_49a4d8', 29, 0xA02840848, DRAM_HUD_MAP_95, [
        (0x4049A4D8,  0, 0xD281091D),
        (0x4049A504, 16, 0xF2A0509D),
        (0x4049A518, 32, 0xF2C0015D),
        (0x4049A538, 48, 0xF2E0001D),
    ]),
    ('reloc_hudmap95_49d310', 19, 0xA02840848, DRAM_HUD_MAP_95, [
        (0x4049D310,  0, 0xD2810913),
        (0x4049D318, 16, 0xF2A05093),
        (0x4049D31C, 32, 0xF2C00153),
        (0x4049D320, 48, 0xF2E00013),
    ]),
    ('reloc_hudmap95_49d794',  8, 0xA02840848, DRAM_HUD_MAP_95, [
        (0x4049D794,  0, 0xD2810908),
        (0x4049D798, 16, 0xF2A05088),
        (0x4049D79C, 32, 0xF2C00148),
        (0x4049D7A0, 48, 0xF2E00008),
    ]),
    # Bug 7b: p=1643 HUD cells (0xA032F6038, 57*24B -> DRAM_HUD_CELLS_95, 95*24B, all 45 refs)
    ('reloc_hudcells95_498934',  9, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x40498934,  0, 0xD28C0709), (0x40498948, 16, 0xF2A065E9), (0x40498958, 32, 0xF2C00149), (0x4049896C, 48, 0xF2E00009)]),
    ('reloc_hudcells95_498b44',  8, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x40498B44,  0, 0xD28C0708), (0x40498B50, 16, 0xF2A065E8), (0x40498B64, 32, 0xF2C00148), (0x40498B74, 48, 0xF2E00008)]),
    ('reloc_hudcells95_498bdc',  9, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x40498BDC,  0, 0xD28C0709), (0x40498BF0, 16, 0xF2A065E9), (0x40498C00, 32, 0xF2C00149), (0x40498C14, 48, 0xF2E00009)]),
    ('reloc_hudcells95_498c68',  9, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x40498C68,  0, 0xD28C0709), (0x40498C7C, 16, 0xF2A065E9), (0x40498C8C, 32, 0xF2C00149), (0x40498CA0, 48, 0xF2E00009)]),
    ('reloc_hudcells95_499050', 22, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x40499050,  0, 0xD28C0716), (0x40499054, 16, 0xF2A065F6), (0x40499058, 32, 0xF2C00156), (0x40499060, 48, 0xF2E00016)]),
    ('reloc_hudcells95_4992f8', 12, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x404992F8,  0, 0xD28C070C), (0x40499308, 16, 0xF2A065EC), (0x40499310, 32, 0xF2C0014C), (0x4049931C, 48, 0xF2E0000C)]),
    ('reloc_hudcells95_499364',  9, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x40499364,  0, 0xD28C0709), (0x40499374, 16, 0xF2A065E9), (0x40499380, 32, 0xF2C00149), (0x40499390, 48, 0xF2E00009)]),
    ('reloc_hudcells95_499554',  9, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x40499554,  0, 0xD28C0709), (0x40499568, 16, 0xF2A065E9), (0x40499578, 32, 0xF2C00149), (0x4049958C, 48, 0xF2E00009)]),
    ('reloc_hudcells95_4998b8', 22, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x404998B8,  0, 0xD28C0716), (0x404998CC, 16, 0xF2A065F6), (0x404998D8, 32, 0xF2C00156), (0x404998E8, 48, 0xF2E00016)]),
    ('reloc_hudcells95_499de4', 13, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x40499DE4,  0, 0xD28C070D), (0x40499DF0, 16, 0xF2A065ED), (0x40499DF4, 32, 0xF2C0014D), (0x40499DF8, 48, 0xF2E0000D)]),
    ('reloc_hudcells95_49a28c',  9, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x4049A28C,  0, 0xD28C0709), (0x4049A298, 16, 0xF2A065E9), (0x4049A2A4, 32, 0xF2C00149), (0x4049A2B4, 48, 0xF2E00009)]),
    ('reloc_hudcells95_49a61c', 26, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x4049A61C,  0, 0xD28C071A), (0x4049A638, 16, 0xF2A065FA), (0x4049A64C, 32, 0xF2C0015A), (0x4049A66C, 48, 0xF2E0001A)]),
    ('reloc_hudcells95_49b370', 19, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x4049B370,  0, 0xD28C0713), (0x4049B380, 16, 0xF2A065F3), (0x4049B38C, 32, 0xF2C00153), (0x4049B398, 48, 0xF2E00013)]),
    ('reloc_hudcells95_49b92c', 10, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x4049B92C,  0, 0xD28C070A), (0x4049B930, 16, 0xF2A065EA), (0x4049B938, 32, 0xF2C0014A), (0x4049B93C, 48, 0xF2E0000A)]),
    ('reloc_hudcells95_49b97c', 10, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x4049B97C,  0, 0xD28C070A), (0x4049B980, 16, 0xF2A065EA), (0x4049B988, 32, 0xF2C0014A), (0x4049B98C, 48, 0xF2E0000A)]),
    ('reloc_hudcells95_49ba0c', 10, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x4049BA0C,  0, 0xD28C070A), (0x4049BA10, 16, 0xF2A065EA), (0x4049BA18, 32, 0xF2C0014A), (0x4049BA1C, 48, 0xF2E0000A)]),
    ('reloc_hudcells95_49bb80',  8, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x4049BB80,  0, 0xD28C0708), (0x4049BB84, 16, 0xF2A065E8), (0x4049BB8C, 32, 0xF2C00148), (0x4049BB90, 48, 0xF2E00008)]),
    ('reloc_hudcells95_49bc08', 10, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x4049BC08,  0, 0xD28C070A), (0x4049BC0C, 16, 0xF2A065EA), (0x4049BC14, 32, 0xF2C0014A), (0x4049BC18, 48, 0xF2E0000A)]),
    ('reloc_hudcells95_49bc88',  8, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x4049BC88,  0, 0xD28C0708), (0x4049BC8C, 16, 0xF2A065E8), (0x4049BC94, 32, 0xF2C00148), (0x4049BC98, 48, 0xF2E00008)]),
    ('reloc_hudcells95_49bcec', 10, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x4049BCEC,  0, 0xD28C070A), (0x4049BCF0, 16, 0xF2A065EA), (0x4049BCF8, 32, 0xF2C0014A), (0x4049BCFC, 48, 0xF2E0000A)]),
    ('reloc_hudcells95_49bda8', 10, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x4049BDA8,  0, 0xD28C070A), (0x4049BDAC, 16, 0xF2A065EA), (0x4049BDB4, 32, 0xF2C0014A), (0x4049BDB8, 48, 0xF2E0000A)]),
    ('reloc_hudcells95_49be94', 10, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x4049BE94,  0, 0xD28C070A), (0x4049BE98, 16, 0xF2A065EA), (0x4049BEA0, 32, 0xF2C0014A), (0x4049BEA4, 48, 0xF2E0000A)]),
    ('reloc_hudcells95_49befc', 10, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x4049BEFC,  0, 0xD28C070A), (0x4049BF00, 16, 0xF2A065EA), (0x4049BF08, 32, 0xF2C0014A), (0x4049BF0C, 48, 0xF2E0000A)]),
    ('reloc_hudcells95_49bf88', 10, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x4049BF88,  0, 0xD28C070A), (0x4049BF8C, 16, 0xF2A065EA), (0x4049BF94, 32, 0xF2C0014A), (0x4049BF98, 48, 0xF2E0000A)]),
    ('reloc_hudcells95_49bfdc', 21, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x4049BFDC,  0, 0xD28C0715), (0x4049BFE0, 16, 0xF2A065F5), (0x4049BFE4, 32, 0xF2C00155), (0x4049BFE8, 48, 0xF2E00015)]),
    ('reloc_hudcells95_49c0ec', 20, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x4049C0EC,  0, 0xD28C0714), (0x4049C0F0, 16, 0xF2A065F4), (0x4049C0F4, 32, 0xF2C00154), (0x4049C0F8, 48, 0xF2E00014)]),
    ('reloc_hudcells95_49c214',  8, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x4049C214,  0, 0xD28C0708), (0x4049C218, 16, 0xF2A065E8), (0x4049C21C, 32, 0xF2C00148), (0x4049C220, 48, 0xF2E00008)]),
    ('reloc_hudcells95_49c29c',  9, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x4049C29C,  0, 0xD28C0709), (0x4049C2A4, 16, 0xF2A065E9), (0x4049C2B0, 32, 0xF2C00149)]),
    ('reloc_hudcells95_49c2cc',  9, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x4049C2CC,  0, 0xD28C0709), (0x4049C2D4, 16, 0xF2A065E9), (0x4049C2E0, 32, 0xF2C00149), (0x4049C2E8, 48, 0xF2E00009)]),
    ('reloc_hudcells95_49c334',  8, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x4049C334,  0, 0xD28C0708), (0x4049C338, 16, 0xF2A065E8), (0x4049C33C, 32, 0xF2C00148), (0x4049C340, 48, 0xF2E00008)]),
    ('reloc_hudcells95_49c3b0',  9, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x4049C3B0,  0, 0xD28C0709), (0x4049C3B4, 16, 0xF2A065E9), (0x4049C3BC, 32, 0xF2C00149), (0x4049C3C0, 48, 0xF2E00009)]),
    ('reloc_hudcells95_49c488',  8, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x4049C488,  0, 0xD28C0708), (0x4049C48C, 16, 0xF2A065E8), (0x4049C490, 32, 0xF2C00148), (0x4049C498, 48, 0xF2E00008)]),
    ('reloc_hudcells95_49c5a8',  8, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x4049C5A8,  0, 0xD28C0708), (0x4049C5AC, 16, 0xF2A065E8), (0x4049C5B0, 32, 0xF2C00148), (0x4049C5B4, 48, 0xF2E00008)]),
    ('reloc_hudcells95_49c68c',  8, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x4049C68C,  0, 0xD28C0708), (0x4049C690, 16, 0xF2A065E8), (0x4049C694, 32, 0xF2C00148), (0x4049C698, 48, 0xF2E00008)]),
    ('reloc_hudcells95_49c8d4', 22, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x4049C8D4,  0, 0xD28C0716), (0x4049C8DC, 16, 0xF2A065F6), (0x4049C8E4, 32, 0xF2C00156), (0x4049C8F4, 48, 0xF2E00016)]),
    ('reloc_hudcells95_49c978',  8, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x4049C978,  0, 0xD28C0708), (0x4049C97C, 16, 0xF2A065E8), (0x4049C980, 32, 0xF2C00148), (0x4049C988, 48, 0xF2E00008)]),
    ('reloc_hudcells95_49cbdc', 10, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x4049CBDC,  0, 0xD28C070A), (0x4049CBE0, 16, 0xF2A065EA), (0x4049CBE8, 32, 0xF2C0014A), (0x4049CBEC, 48, 0xF2E0000A)]),
    ('reloc_hudcells95_49d084',  8, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x4049D084,  0, 0xD28C0708), (0x4049D088, 16, 0xF2A065E8), (0x4049D08C, 32, 0xF2C00148), (0x4049D090, 48, 0xF2E00008)]),
    ('reloc_hudcells95_49e670', 19, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x4049E670,  0, 0xD28C0713), (0x4049E680, 16, 0xF2A065F3), (0x4049E68C, 32, 0xF2C00153), (0x4049E698, 48, 0xF2E00013)]),
    ('reloc_hudcells95_49eaf4', 11, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x4049EAF4,  0, 0xD28C070B), (0x4049EAF8, 16, 0xF2A065EB), (0x4049EB00, 32, 0xF2C0014B), (0x4049EB04, 48, 0xF2E0000B)]),
    ('reloc_hudcells95_49ec88',  9, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x4049EC88,  0, 0xD28C0709), (0x4049EC8C, 16, 0xF2A065E9), (0x4049EC98, 32, 0xF2C00149), (0x4049EC9C, 48, 0xF2E00009)]),
    ('reloc_hudcells95_49f744',  9, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x4049F744,  0, 0xD28C0709), (0x4049F748, 16, 0xF2A065E9), (0x4049F750, 32, 0xF2C00149), (0x4049F758, 48, 0xF2E00009)]),
    ('reloc_hudcells95_49f980',  9, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x4049F980,  0, 0xD28C0709), (0x4049F984, 16, 0xF2A065E9), (0x4049F98C, 32, 0xF2C00149), (0x4049F990, 48, 0xF2E00009)]),
    ('reloc_hudcells95_49fa0c', 26, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x4049FA0C,  0, 0xD28C071A), (0x4049FA10, 16, 0xF2A065FA), (0x4049FA14, 32, 0xF2C0015A), (0x4049FA1C, 48, 0xF2E0001A)]),
    ('reloc_hudcells95_4a00f0',  9, 0xA032F6038, DRAM_HUD_CELLS_95, [(0x404A00F0,  0, 0xD28C0709), (0x404A00F4, 16, 0xF2A065E9), (0x404A00FC, 32, 0xF2C00149), (0x404A0100, 48, 0xF2E00009)]),
]


def apply(image: bytearray) -> dict:
    """Validate all pre-patch guards and apply all 8 C64 EL1 UI fixes in-place.

    All assertions run BEFORE any byte of `image` is mutated.
    Returns a JSON-serializable metadata dictionary describing every patch site,
    RX cave utilization, and C64 DRAM scratch allocation.
    """
    if not isinstance(image, bytearray):
        raise TypeError('image must be a mutable bytearray')

    code_end = CODE_VA + len(CODE_PAYLOAD)
    ext_table_end = EXT_TILE_TABLE_VA + EXT_TILE_BYTES
    if not (CAVE_GUARD_START <= CAVE_START <= EXT_TILE_TABLE_VA and ext_table_end <= CODE_VA and code_end <= CAVE_LIMIT):
        raise AssertionError('cave layout overflow')

    # 1. Assert entire C64 RX tail cave 0x40B65F18..0x40B70000 is 100% zero
    cave_off_start = CAVE_GUARD_START - BASE
    cave_off_end = CAVE_LIMIT - BASE
    if cave_off_end > len(image):
        raise AssertionError('image too small for C64 RX tail cave')
    if any(image[cave_off_start:cave_off_end]):
        first_nz = next(i for i, b in enumerate(image[cave_off_start:cave_off_end]) if b)
        raise AssertionError(
            f'C64 RX tail cave not zero at {CAVE_GUARD_START + first_nz:#x}'
        )

    # 2. Validate & stage extended 383-entry 12-grid UI table (Bug 2B)
    stock_tbl_off = DATA_FILE_OFF + (STOCK_TILE_TABLE_RAM - DATA_RAM_BASE)
    stock_tbl_bytes = bytes(image[stock_tbl_off:stock_tbl_off + STOCK_TILE_COUNT * EXT_TILE_ENTRY_SIZE])
    first_key, first_f1, first_res, first_f3, first_ptr = struct.unpack_from('<IIIIQ', stock_tbl_bytes, 0)
    last_key, last_f1, last_res, last_f3, last_ptr = struct.unpack_from(
        '<IIIIQ', stock_tbl_bytes, (STOCK_TILE_COUNT - 1) * EXT_TILE_ENTRY_SIZE
    )
    if (first_key, first_f1, first_res) != (0x00, 1, 0x07C5):
        raise AssertionError(f'unexpected 0xA028718C0[0]: {(first_key, first_f1, first_res)}')
    if (last_key, last_f1, last_res) != (0x6B, 1, 0x0AC4):
        raise AssertionError(f'unexpected 0xA028718C0[107]: {(last_key, last_f1, last_res)}')

    ext_table = bytearray(stock_tbl_bytes)
    for slot in range(10, 65):
        idx = slot - 10
        for var in range(C64_TILE_VARIANTS):
            key = STOCK_TILE_COUNT + idx * C64_TILE_VARIANTS + var
            res_id = 0x7E00 + idx * C64_TILE_VARIANTS + var
            ext_table += struct.pack('<IIIIQ', key, 1, res_id, 0, 0)
    assert len(ext_table) == EXT_TILE_BYTES

    # 3. Stage & pre-validate EVERY patch edit before mutating image
    staged: List[Tuple[str, int, bytes, bytes]] = []

    # 3a. Branch hooks
    for name, va, old_hex, sym in BRANCH_HOOKS:
        old_bytes = bytes.fromhex(old_hex)
        new_bytes = _encode_b(va, SYMBOLS[sym])
        actual = bytes(image[va - BASE:va - BASE + len(old_bytes)])
        if actual != old_bytes:
            raise AssertionError(
                f'{name} @ {va:#x}: expected {old_bytes.hex()}, got {actual.hex()}'
            )
        staged.append((name, va, old_bytes, new_bytes))

    # 3b. Direct edits (cmp bounds & descriptors)
    for name, va, old_bytes, new_bytes in DIRECT_EDITS:
        actual = bytes(image[va - BASE:va - BASE + len(old_bytes)])
        if actual != old_bytes:
            raise AssertionError(
                f'{name} @ {va:#x}: expected {old_bytes.hex()}, got {actual.hex()}'
            )
        staged.append((name, va, old_bytes, new_bytes))

    # 3c. Pointer relocations
    reloc_meta = []
    for name, reg, old_va, new_va, slots in POINTER_RELOCS:
        recon = 0
        slot_entries = []
        for insn_va, shift, expected_u32 in slots:
            old_bytes = struct.pack('<I', expected_u32)
            actual = bytes(image[insn_va - BASE:insn_va - BASE + 4])
            if actual != old_bytes:
                raise AssertionError(
                    f'{name} @ {insn_va:#x}: expected {old_bytes.hex()}, got {actual.hex()}'
                )
            imm_old = (expected_u32 >> 5) & 0xFFFF
            recon |= imm_old << shift
            imm_new = (new_va >> shift) & 0xFFFF
            new_bytes = _encode_movz(reg, imm_new, shift) if shift == 0 else _encode_movk(reg, imm_new, shift)
            staged.append((f'{name}_s{shift}', insn_va, old_bytes, new_bytes))
            slot_entries.append({'va': hex(insn_va), 'shift': shift, 'before': old_bytes.hex(), 'after': new_bytes.hex()})
        if recon != old_va:
            raise AssertionError(f'{name}: reconstructed {recon:#x} != {old_va:#x}')
        reloc_meta.append({
            'name': name,
            'reg': f'x{reg}',
            'old_va': hex(old_va),
            'new_va': hex(new_va),
            'instructions': slot_entries,
        })

    # 3d. Check no overlapping spans among staged edits
    spans = sorted((va, va + len(nb), name) for name, va, _, nb in staged)
    for (s0, e0, n0), (s1, e1, n1) in zip(spans, spans[1:]):
        if e0 > s1:
            raise AssertionError(f'overlapping edits: {n0} ({s0:#x}..{e0:#x}) and {n1} ({s1:#x}..{e1:#x})')

    # 4. Commit all mutations atomically now that 100% of assertions passed
    in_sha256 = _sha256(image)

    # Write extended 383-entry table and machine code into owned RX tail cave
    image[EXT_TILE_TABLE_VA - BASE:ext_table_end - BASE] = ext_table
    image[CODE_VA - BASE:code_end - BASE] = CODE_PAYLOAD

    # Write hook/direct/relocation edits
    site_meta = []
    for name, va, old_bytes, new_bytes in staged:
        image[va - BASE:va - BASE + len(new_bytes)] = new_bytes
        site_meta.append({
            'name': name,
            'va': hex(va),
            'size': len(new_bytes),
            'before': old_bytes.hex(),
            'after': new_bytes.hex(),
        })

    # Post-check: verify zero bytes remain untouched before CAVE_START and after code_end
    assert not any(image[CAVE_GUARD_START - BASE:CAVE_START - BASE]), 'cave guard prefix dirty'
    assert not any(image[ext_table_end - BASE:CODE_VA - BASE]), 'inter-section alignment padding dirty'
    assert not any(image[code_end - BASE:CAVE_LIMIT - BASE]), 'cave tail dirty'

    return {
        'module': 'patch_c64_imenu_ui',
        'input_sha256': in_sha256,
        'output_sha256': _sha256(image),
        'build_id_recalculated': False,
        'rx_cave': {
            'owned_range': [hex(CAVE_GUARD_START), hex(CAVE_LIMIT)],
            'owned_bytes': CAVE_LIMIT - CAVE_GUARD_START,
            'ext_tile_table': {
                'va': hex(EXT_TILE_TABLE_VA),
                'end': hex(ext_table_end),
                'entries': EXT_TILE_COUNT,
                'bytes': EXT_TILE_BYTES,
            },
            'code': {
                'va': hex(CODE_VA),
                'end': hex(code_end),
                'exec_end': hex(0x40B698CC),
                'exec_bytes': 0x40B698CC - CODE_VA,
                'rodata_bytes': code_end - 0x40B698CC,
                'bytes': len(CODE_PAYLOAD),
                'symbols': {k: hex(v) for k, v in SYMBOLS.items()},
            },
            'used_span': [hex(CAVE_START), hex(code_end)],
            'used_bytes': code_end - CAVE_START,
            'free_remaining_bytes': CAVE_LIMIT - code_end,
            'fp_simd_count': 0,
        },
        'dram_allocations': {
            'new_map_95':      {'va': hex(DRAM_NEW_MAP_95),     'bytes': 95 * 8,  'replaces': '0xa0283b2e8'},
            'new_cells_2a':    {'va': hex(DRAM_NEW_CELLS_2A),   'bytes': 95 * 24, 'replaces': '0xa028b5eb8'},
            's2_valid':        {'va': hex(DRAM_S2_VALID),       'bytes': 4,       'purpose': 'p=1562 state2 adjust guard'},
            'fn_pc_table_95':  {'va': hex(DRAM_FN_PC_TABLE_95), 'bytes': 95 * 16, 'replaces': '0xa02822d48'},
            'fn_cand_buf':     {'va': hex(DRAM_FN_CAND_BUF),    'bytes': 0x180,   'replaces': '0xa0 stack buf @ 0x40d39a20'},
            'hud_map_95':      {'va': hex(DRAM_HUD_MAP_95),     'bytes': 95 * 8,  'replaces': '0xa02840848'},
            'hud_cells_95':    {'va': hex(DRAM_HUD_CELLS_95),   'bytes': 95 * 24, 'replaces': '0xa032f6038'},
            'png_icon_cache':  {'va': hex(DRAM_PNG_CACHE),      'bytes': 16 * 0xB00, 'purpose': '16-slot native kind=1 64x32 + 68x36 LV outline PNG cache'},
            'total_bytes':     95 * 8 + 95 * 24 + 4 + 95 * 16 + 0x180 + 95 * 8 + 95 * 24 + 16 * 0xB00,
        },
        'branch_hooks_count': len(BRANCH_HOOKS),
        'direct_edits_count': len(DIRECT_EDITS),
        'pointer_relocs_count': len(POINTER_RELOCS),
        'pointer_relocs': reloc_meta,
        'patched_sites': site_meta,
    }


def _recalc_build_id(image: bytearray) -> str:
    """Recalculate Nikon V71 bootguard build ID at 0x401FE060 (test harness use only)."""
    bid_off = 0x401FE060 - BASE
    image[bid_off:bid_off + 16] = b'\x00' * 16
    digest = hashlib.sha256(image).digest()[:16]
    image[bid_off:bid_off + 16] = digest
    return digest.hex()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--input', type=Path, required=True, help='Input MAIN firmware image')
    ap.add_argument('--output', type=Path, help='Output patched MAIN image')
    ap.add_argument('--meta', type=Path, help='Optional JSON receipt path')
    ap.add_argument('--recalc-build-id', action='store_true',
                    help='Recalculate 0x401FE060 build ID after patching (for standalone QEMU test)')
    args = ap.parse_args()

    img = bytearray(args.input.read_bytes())
    meta = apply(img)
    if args.recalc_build_id:
        meta['build_id'] = _recalc_build_id(img)
        meta['build_id_recalculated'] = True
        meta['output_sha256'] = _sha256(img)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(img)
    if args.meta:
        args.meta.parent.mkdir(parents=True, exist_ok=True)
        args.meta.write_text(json.dumps(meta, indent=2) + '\n')
    print(json.dumps({
        'status': 'OK',
        'input': str(args.input),
        'output': str(args.output) if args.output else None,
        'rx_used_bytes': meta['rx_cave']['used_bytes'],
        'rx_code_bytes': meta['rx_cave']['code']['bytes'],
        'rx_table_bytes': meta['rx_cave']['ext_tile_table']['bytes'],
        'rx_free_remaining': meta['rx_cave']['free_remaining_bytes'],
        'dram_bytes': meta['dram_allocations']['total_bytes'],
        'sites_patched': len(meta['patched_sites']),
        'build_id_recalculated': meta['build_id_recalculated'],
        'output_sha256': meta['output_sha256'],
    }, indent=2))


if __name__ == '__main__':
    main()
