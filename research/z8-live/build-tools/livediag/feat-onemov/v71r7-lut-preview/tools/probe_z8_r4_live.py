#!/usr/bin/env python3
"""Read-only Nikon Z8 V71.7-r4 Live-Silicon USB Telemetry Probe (Layer0Metrics v12 = 416 B).

Connects over USB PTP/Fenestra debug bridge on Windows (`win-lab` or local host with
Fenestra Python client) and captures two snapshots separated by `--interval` seconds
to compute real-time LCD/EVF FPS, per-stage timing breakdown (copy/LUT/HDF/grain/clean),
Single-Pass Live->LCD reuse hit rate, SMP helper status, XM6 Core 0 stats, and
Pool 0 / display reject / force-retire stability counters.

Usage (on win-lab or any host with Fenestra USB client):
  python3 probe_z8_r4_live.py --interval 3.0 --out real_silicon_usb_probe_r4.json
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
import time
from pathlib import Path

V71R7_DRAM_ANCHOR_VA = 0xA11640000
V71R7_DRAM_IO_GATE_VA = 0xA11640040
V71R7_DRAM_FLIGHT_HEAD_VA = 0xA11641800
V71R7_DRAM_WATCHDOG_VA = 0xA11641908
V71R7_ANCHOR_MAGIC = 0x56373137
V71R7_LAYER0_CTX_MAGIC = 0x4C305637
V71R7_LUT_CTX_MAGIC = 0x4C555437

ADDR_BUILD_ID = 0x401FE060
ADDR_PANEL_BANK = 0xA0306C4D8
ADDR_DS_2126 = 0xA03D7C7A4
ADDR_XM6_STATE = 0xA02E7E734
ADDR_XM6_LOADED = 0xA02E803E8
ADDR_XM6_FAULT = 0xA02E803EE
ADDR_XM6_MBOX = 0xA02E7EED0
ADDR_CSL1 = 0xA1171B000
ADDR_STOCK_ROOT = 0xA032B50D8


def valid_ram(a: int, n: int = 8) -> bool:
    return (a & 3) == 0 and 0xA00000000 <= a <= 0xA40000000 - n


def decode_layer0_v12(raw: bytes) -> dict:
    """Decode Layer0Ctx header (8 B) + Layer0Metrics v12 (416 B)."""
    magic, sz = struct.unpack_from("<II", raw, 0)
    if magic != V71R7_LAYER0_CTX_MAGIC:
        return {"magic": hex(magic), "magic_ok": False}
    m = raw[8 : 8 + 416]
    u32 = lambda o: struct.unpack_from("<I", m, o)[0]
    i32 = lambda o: struct.unpack_from("<i", m, o)[0]
    u64 = lambda o: struct.unpack_from("<Q", m, o)[0]
    d = {
        "magic_ok": True,
        "ctx_size": sz,
        "version": u32(0),
        "bytes": u32(4),
        "hw_current": i32(8),
        "active_banks_mask": u32(12),
        "disp_calls": u32(16),
        "disp_substituted": u32(20),
        "disp_kept_current": u32(24),
        "disp_stock_pass": u32(28),
        "disp_rejects": u32(32),
        "vsync_ticks": u32(36),
        "banks_retired": u32(40),
        "enqueue_seen": u32(44),
        "enqueue_copied": u32(48),
        "enqueue_skipped_busy": u32(52),
        "enqueue_rejected_fmt": u32(56),
        "enqueue_content_changed": u32(60),
        "copy_us": u32(64),
        "max_copy_us": u32(68),
        "grades": u32(72),
        "grade_us": u32(76),
        "max_grade_us": u32(80),
        "grade_timeouts": u32(84),
        "display_mode": u32(88),
        "copy_mode": u32(92),
        "last_latency_us": u32(96),
        "max_latency_us": u32(100),
        "last_interframe_ms": u32(104),
        "max_interframe_ms": u32(108),
        "distinct_sources": u32(112),
        "repeated_sources": u32(116),
        "errors": u32(120),
        "last_result": i32(124),
        "pool_result": i32(128),
        "online": u32(132),
        "enabled": u32(136),
        "display_ready": u32(140),
        "mode_ok": u32(144),
        "hook_active": u32(148),
        "quarantined": u32(152),
        "lut_bytes": u32(156),
        "grid": u32(160),
        "surface_bytes": u32(164),
        "disp_shadow_flag": u32(168),
        "stock_baseline_confirmed": u32(172),
        "source_lag": u32(176),
        "mismatch_y": u32(180),
        "mismatch_uv": u32(184),
        "mismatch_meta": u32(188),
        "last_mismatch_row": u32(192),
        "step_bursts": u32(196),
        "last_captured_ms": u64(200),
        "last_presented_ms": u64(208),
        "last_source_stamp": u64(216),
        "last_sub_raw_y": hex(u64(224)),
        "last_stock_y": hex(u64(232)),
        "vsync_seq": u64(240),
        "last_graded_ms": u64(248),
        "last_presented_generation": u64(256),
        "sentinel_status": u32(264),
        "neon_active": u32(268),
        "active_route": u32(272),
        "lcd_substituted": u32(276),
        "evf_substituted": u32(280),
        "worker_cores": u32(284),
        "fps_milli": u32(288),
        "capture_aborts": u32(292),
        "lcd_hw_current": i32(296),
        "evf_hw_current": i32(300),
        "evf_vsync_seq": u64(304),
        "helper_sentinel": [u32(312), u32(316), u32(320)],
        "wait_no_source": u32(328),
        "wait_no_bank": u32(332),
        "vsync_claim_misses": u32(336),
        "cross_route_retires": u32(340),
        "last_sleep_us": u32(344),
        "max_sleep_us": u32(348),
        "frame_us": u32(352),
        "max_frame_us": u32(356),
        "overlapped_frames": u32(360),
        "jpeg_pauses": u32(364),
        "deferred_joins": u32(368),
    }
    if len(raw) >= 8 + 416 and d["version"] >= 12:
        d.update({
            "last_copy_us": u32(376),
            "last_lut_us": u32(380),
            "last_hdf_us": u32(384),
            "last_grain_us": u32(388),
            "last_clean_us": u32(392),
            "live_reuse_hits": u32(396),
            "live_reuse_misses": u32(400),
            "smp_helpers_online": u32(404),
            "force_retires": u32(408),
        })
    if len(raw) >= 8 + 424:
        d["pool_lock_waits"], d["pool_lock_wait_ok"] = struct.unpack_from("<II", raw, 8 + 416)
    return d


def take_snapshot(read_mem) -> dict:
    rd = lambda a, n: int.from_bytes(read_mem(a, n), "little")
    snap: dict[str, object] = {
        "ts": time.time(),
        "build_id": hex(rd(ADDR_BUILD_ID, 4)),
        "panel_bank": rd(ADDR_PANEL_BANK, 4),
        "ds_2126": rd(ADDR_DS_2126, 4),
        "xm6_hw_state": rd(ADDR_XM6_STATE, 4),
        "xm6_fw_loaded": rd(ADDR_XM6_LOADED, 1),
        "xm6_loader_fault": rd(ADDR_XM6_FAULT, 1),
        "xm6_mailbox": [hex(w) for w in struct.unpack("<4I", read_mem(ADDR_XM6_MBOX, 16))],
        "csl1": {
            "magic": hex(rd(ADDR_CSL1, 4)),
            "drive": chr(rd(ADDR_CSL1 + 4, 1)) if rd(ADDR_CSL1 + 4, 1) in (65, 66) else rd(ADDR_CSL1 + 4, 1),
        },
    }
    anc = read_mem(V71R7_DRAM_ANCHOR_VA, 64)
    magic, flags, lut_ptr, l0_ptr, menu_ptr, permit, gen, xm6_ptr = struct.unpack_from("<IIQQQIIQ", anc, 0)
    lut_master, lut_mask, xm6_m, lut_set, lut_epoch = struct.unpack_from("<BBBBI", anc, 48)
    snap["anchor"] = {
        "magic": hex(magic),
        "flags": hex(flags),
        "lut_ctx": hex(lut_ptr),
        "l0_ctx": hex(l0_ptr),
        "menu_ctx": hex(menu_ptr),
        "xm6_bridge": hex(xm6_ptr),
        "output_permit": permit,
        "active_generation": gen,
        "lut_master_on": lut_master,
        "lut_stage_mask": lut_mask,
        "xm6_mode": xm6_m,
        "lut_cfg_set": lut_set,
        "lut_enable_epoch": lut_epoch,
    }
    if valid_ram(l0_ptr, 440):
        snap["layer0"] = decode_layer0_v12(read_mem(l0_ptr, 432))
    if valid_ram(lut_ptr, 64):
        lc_b = read_mem(lut_ptr, 64)
        lc_magic, _, active, lgen, is_1d, xm6_mode = struct.unpack_from("<IIIIiB", lc_b, 0)
        snap["lut_core"] = {
            "magic_ok": (lc_magic == V71R7_LUT_CTX_MAGIC),
            "active": active,
            "generation": lgen,
            "is_1d_separable": is_1d,
            "xm6_mode": xm6_mode,
        }
    if valid_ram(xm6_ptr, 0x80):
        xb = read_mem(xm6_ptr, 0x80)
        snap["xm6_bridge"] = {
            "xm6_busy": struct.unpack_from("<I", xb, 0x10)[0],
            "breaker_tripped": struct.unpack_from("<I", xb, 0x14)[0],
            "last_err": struct.unpack_from("<I", xb, 0x2C)[0],
            "last_dsp_status": hex(struct.unpack_from("<I", xb, 0x30)[0]),
            "last_still_ms": struct.unpack_from("<I", xb, 0x34)[0],
            "last_live_us": struct.unpack_from("<I", xb, 0x38)[0],
            "still_ok": struct.unpack_from("<I", xb, 0x3C)[0],
            "live_ok": struct.unpack_from("<I", xb, 0x40)[0],
            "fallback_cnt": struct.unpack_from("<I", xb, 0x44)[0],
        }
    wd = read_mem(V71R7_DRAM_WATCHDOG_VA, 32)
    wd_magic, wd_stalled, wd_dumps, wd_err = struct.unpack_from("<4I", wd, 0)
    snap["flight_head"] = rd(V71R7_DRAM_FLIGHT_HEAD_VA, 8)
    snap["watchdog"] = {
        "magic": hex(wd_magic),
        "stalled_slots": wd_stalled,
        "dumps_written": wd_dumps,
        "last_err": wd_err,
    }
    return snap


def compute_delta(s1: dict, s2: dict) -> dict:
    dt = max(1e-6, s2["ts"] - s1["ts"])
    l1, l2 = s1.get("layer0") or {}, s2.get("layer0") or {}
    x1, x2 = s1.get("xm6_bridge") or {}, s2.get("xm6_bridge") or {}

    def diff(a, b, k):
        return (b.get(k, 0) - a.get(k, 0)) if (k in a and k in b) else None

    d_grades = diff(l1, l2, "grades") or 0
    d_sub = diff(l1, l2, "disp_substituted") or 0
    d_kept = diff(l1, l2, "disp_kept_current") or 0
    d_stock = diff(l1, l2, "disp_stock_pass") or 0
    d_hits = diff(l1, l2, "live_reuse_hits") or 0
    d_miss = diff(l1, l2, "live_reuse_misses") or 0
    d_xm6_live = diff(x1, x2, "live_ok") or 0
    d_xm6_fb = diff(x1, x2, "fallback_cnt") or 0
    return {
        "dt_s": round(dt, 3),
        "route": "EVF" if s2.get("panel_bank") == 1 else "LCD",
        "grades_fps": round(d_grades / dt, 2),
        "disp_sub_fps": round(d_sub / dt, 2),
        "disp_kept_delta": d_kept,
        "disp_stock_delta": d_stock,
        "disp_rejects_delta": diff(l1, l2, "disp_rejects"),
        "force_retires_delta": diff(l1, l2, "force_retires"),
        "live_reuse_hits_delta": d_hits,
        "live_reuse_misses_delta": d_miss,
        "live_reuse_hit_pct": round(100.0 * d_hits / (d_hits + d_miss), 1) if (d_hits + d_miss) else None,
        "xm6_live_ok_per_s": round(d_xm6_live / dt, 2),
        "xm6_fallback_delta": d_xm6_fb,
        "last_stage_us": {
            "copy": l2.get("last_copy_us"),
            "lut": l2.get("last_lut_us"),
            "hdf": l2.get("last_hdf_us"),
            "grain": l2.get("last_grain_us"),
            "clean": l2.get("last_clean_us"),
            "frame": l2.get("frame_us"),
            "latency": l2.get("last_latency_us"),
            "max_sleep": l2.get("max_sleep_us"),
        },
        "worker_cores": l2.get("worker_cores"),
        "smp_helpers_online": l2.get("smp_helpers_online"),
        "helper_sentinel": l2.get("helper_sentinel"),
        "pool_lock_waits_delta": diff(l1, l2, "pool_lock_waits"),
        "pool_lock_wait_ok_delta": diff(l1, l2, "pool_lock_wait_ok"),
    }


def main() -> None:
    ap = argparse.ArgumentParser(description="Nikon Z8 V71.7-r4 Live-Silicon USB Telemetry Probe (v12 416B)")
    ap.add_argument("--interval", type=float, default=3.0, help="Seconds between s1 and s2 snapshots")
    ap.add_argument("--out", type=Path, default=None, help="Optional JSON output path")
    ap.add_argument("--fenestra-tools", type=Path, default=None, help="Path containing fenestra_usb / fx_client module")
    args = ap.parse_args()

    if args.fenestra_tools and str(args.fenestra_tools) not in sys.path:
        sys.path.insert(0, str(args.fenestra_tools))
    # Auto-discover Fenestra USB client from standard WinLab locations
    for cand in (
        Path(r"C:\Users\ycx\fenestra-tools"),
        Path(r"C:\fenestra"),
        Path.home() / "fenestra-tools",
    ):
        if cand.is_dir() and str(cand) not in sys.path:
            sys.path.insert(0, str(cand))

    try:
        from fenestra_usb import FenestraUsbClient  # type: ignore
    except ImportError as exc:
        print(
            "ERROR: Could not import fenestra_usb.FenestraUsbClient.\n"
            "Pass --fenestra-tools <dir> on win-lab, or import decode_layer0_v12/take_snapshot/compute_delta "
            "directly from this module with any read_mem(addr, size) callback!",
            file=sys.stderr,
        )
        raise SystemExit(2) from exc

    with FenestraUsbClient() as cli:
        hello = cli.hello()
        s1 = take_snapshot(cli.read_mem)
        time.sleep(max(0.2, args.interval))
        s2 = take_snapshot(cli.read_mem)
    out = {"hello": hello, "s1": s1, "s2": s2, "delta": compute_delta(s1, s2)}
    txt = json.dumps(out, indent=2) + "\n"
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(txt, encoding="utf-8")
    print(txt)


if __name__ == "__main__":
    main()
