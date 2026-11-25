# V69 All-in-One FilmSim EL1 Integration Guide (`src-fix37` Hooks & `.filmsim` Bank)

This bundle integrates **Ricoh Optical HDF (`z8_hdf`)**, **Nikon Zf C3.01 18-level Bit-Exact Film Grain (`z8_zf_grain`)**, and **Fujifilm Color Chrome / FX Blue / 33³ `.z8lut` (`z8_film_engine`)** into V69 All-in-One (`V68 Duplex30fps` + `HangFix` + `PortraitPB` + `FontCJK-ZhUI`) with **zero conflicts**.

---

## 1. Memory & Bank Layout (Verified Zero-Conflict)

| Region | Address Range | Used / Cap | Notes |
| :--- | :--- | ---: | :--- |
| `playback` | `0x40196000..0x4019F020` | — | **0 B touched** by FilmSim |
| `shutter` | `0x4019F020..0x4019FF40` | — | **0 B touched** by FilmSim |
| `Fenestra Core` | `0x401A0000..0x401E2000` | — | **0 B touched** by FilmSim (reads `FxModuleBus` passively via `0x401ccde4`, `0x401e0f38`, `0x401ac208`) |
| `live_primary` (`src-fix37`) | `0x401E2000..0x401FCD00` | +~400–500 B | Stays **in place**; ends strictly `< 0x401FCD00` (leaves PortraitPB cave `0x401FCD00..0x401FCE8C` clear) |
| `live_extra` | `0x401FD218..0x401FE000` | — | **0 B touched** by FilmSim |
| `FontCJK CJK_EXT_1` | `0x408C6000..0x40951000` | — | **0 B touched** by FilmSim (avoided `v66-filmsim-wlm-r2`'s old `0x408C6000` bank!) |
| **`.filmsim` RX Bank** | **`0x409B0000..0x409D8000`** | **`85,200 B` / `163,840 B`** | **`78,640 B` (`76.8 KiB`) free headroom**; formally audited in `tests/filmsim/bank_audit.json` (`0` Stock hits, `0` V68 code hits, `0` `.data`/`.bss`, `0` FP insns anywhere, NEON strictly in 3 kernel `.o`s) |

---

## 2. Applying the 4 Tiny `src-fix37` Hooks

Exact unified diff against `v68-duplex30fps/work/src-fix37/`:
- Patch file: [src_fix37_filmsim.patch](file:///Users/ycx/Nikon/build-tools/livediag/feat-onemov/v69-allinone/work/filmsim/src_fix37_filmsim.patch) (verified `patch -p1 --dry-run -F0` clean on V68 `src-fix37`).

Summary of the 4 files touched (add `-I .../work/filmsim/src/runtime -I .../work/filmsim/src/include` to `src-fix37` compile flags):

1. **`ui/live_ui.h`**:
   - `#include <stdatomic.h>`
   - Declare `_Atomic(void *) *live_ui_filmsim_context_slot(void);`
2. **`ui/live_ui.c`**:
   - `#include "filmsim_runtime.h"`
   - Append `_Atomic(void *) filmsim_context;` at the **end** of `UiState` (all earlier offsets unchanged).
   - Implement `live_ui_filmsim_context_slot()` returning `&u->filmsim_context`.
   - In `live_ui_poll()`: call `filmsim_runtime_on_stop();` when `u->shutdown` is set, and call `filmsim_runtime_ui_poll();` on normal ticks.
3. **`runtime.c`**:
   - `#include "filmsim_runtime.h"`
   - In `live_runtime_event()`: call `filmsim_runtime_on_stop()` on `event == 7` (shutdown) and `filmsim_runtime_on_card_change()` on `event == 24` (card swap).
   - In `live_finalize_wrap()`: apply the `RAW+JPEG fd=0` ticket-reuse fix (`q->open && !q->closed && q->fd==fd && q->file_id==id && q->card_epoch==epoch`, `count == 1`).
   - **Recommended HangFix (`class 0x24`) coordination**:
     - When `@hang-bug`'s HangFix disposes `class 0x24` on Photo→Movie / Playback transitions, call **`filmsim_runtime_pool_suspend()`** so an active FilmSim `arena_blk` (`class 0x24`) is cleanly released before `0x410463e4` runs (preventing `T[0x24].byte2 = 1` pool poisoning).
     - When Photo mode resumes (`live_start_notify` / Photo mode re-entry), call **`filmsim_runtime_pool_resume()`** so FilmSim lazily re-acquires `arena_blk` on the next `filmsim_runtime_ui_poll()`.
     - *(When FilmSim is Dormant/Off, both functions are immediate no-ops.)*
4. **`capture/live_stream.c`**:
   - `#include "filmsim_runtime.h"`
   - Update `preview_push(s, pts, frame)` to take `const uint8_t *frame`.
   - Call `filmsim_runtime_live_submit(s->i420, s->width, s->height, stamp)` right after a valid frame copy.
   - Around `live_encoder_frame_timed()` + `preview_push()`: non-blocking `filmsim_runtime_live_active()` / `filmsim_runtime_live_acquire()` / `filmsim_runtime_live_release()` (zero CPU0 NEON, zero CPU0 stall).
   - **Merge note with `@hang-bug`**: Line 714 in `src_fix37_filmsim.patch` has V68's `if(tick>s->cfr_source_us+33367u)s->cfr_repeats++;` as context — preserve `@hang-bug`'s 1-line telemetry fix `if(s->cfr_have==2u)s->cfr_repeats++;` when merging onto `v69-allinone/work/src-fix37/`.

---

## 3. Compiling & Linking the 18 FilmSim Units into `.filmsim` (`0x409B0000`)

1. Compile the 18 objects using [build_filmsim_objs.sh](file:///Users/ycx/Nikon/build-tools/livediag/feat-onemov/v69-allinone/work/filmsim/build_filmsim_objs.sh):
   ```sh
   SRC_FIX37_DIR=/path/to/v69-allinone/work/src-fix37 \
   FENESTRA_INC_DIR=/Users/ycx/Nikon/fenestra-z8/include \
   OUT_DIR=/path/to/build/objects \
   CC=clang-21 \
     /path/to/v69-allinone/work/filmsim/build_filmsim_objs.sh
   ```
   - Produces `filmsim_01-*.o` .. `filmsim_18-*.o` (15 scalar `-mgeneral-regs-only` + 3 selective NEON kernel units).
2. Include [filmsim_section.ld.inc](file:///Users/ycx/Nikon/build-tools/livediag/feat-onemov/v69-allinone/work/filmsim/filmsim_section.ld.inc) in `payload.ld`:
   ```ld
   . = 0x401e2000;
   .text   : { *(EXCLUDE_FILE(*filmsim_*.o) .text*) }
   .rodata : { *(EXCLUDE_FILE(*filmsim_*.o) .rodata*) }
   ASSERT(. <= 0x401fcd00, "live_primary collides with PortraitPB cave 0x401FCD00")

   .filmsim 0x409b0000 : {
     *filmsim_*.o(.text*)
     . = ALIGN(16);
     *filmsim_*.o(.rodata*)
   }
   ASSERT(. <= 0x409d8000, "FilmSim RX bank 0x409B0000..0x409D8000 exhausted")
   ```
3. Splice `.filmsim` binary (`0x409B0000..0x409C4CD0`, `85,200 B`) into `main.main`, then patch the 14 stock photo hooks with [patch_photo_v69.py](file:///Users/ycx/Nikon/build-tools/livediag/feat-onemov/v69-allinone/work/filmsim/patch_photo_v69.py):
   ```sh
   python3 patch_photo_v69.py --main stage1.main --symbols payload.elf --out stage1_photo.main --out-json photo_hooks.json
   ```
   - Routes `Job 2` (`0x40eaf554`, fixed `7.04 MiB` NEF Basic preview) to `z8_np_engine19_job2_stub` (`Z8_NFSP_SLOT_NEF_PREVIEW = 1`), guaranteeing zero hardware `err 85` overflow when any Zf or Fuji grain preset is active while `Job 1` (`0x40eaeb8c`, Main Fine★ JPEG, `29.5 MB`) receives the full HDF + Fuji + Zf Grain pipeline.

---

## 4. SD Card Assets (`v69-allinone/sd-card/FENESTRA/`)

- `FENESTRA/filmsim.wlm` (`18,900 B`, SHA-256 `60ba3814e07538803c79f566361e7ed08e9a9746c9c18829aefa533bd4ae0e68`)
- `FENESTRA/ZF_GRAIN.BIN` (`27,588 B`, SHA-256 `99c70fa224b701d34621edd159be09b3536f00b45173ac07e57b12cdaa18433d`)
- Optional user LUTs: `FENESTRA/LUTS/LUT01.Z8LUT..LUT16.Z8LUT` (`215,680 B` each)
- Optional Picture Controls: `NIKON/CUSTOMPC/PICCON01.NP3..`
