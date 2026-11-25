# Fenestra for Nikon Z8 (`C3.11`)

**English** | [简体中文](README.zh-CN.md) · [Detailed V71.7-r7 Feature & Menu Reference (EN)](docs/features-and-usage.md) | [V71.7-r7 详细功能与全菜单设置手册 (简体中文)](docs/features-and-usage.zh-CN.md)

`fenestra-z8` is the **Nikon Z8 (firmware C3.11)** adaptation of [WonderLantern/Fenestra](https://github.com/WonderLantern/Fenestra) (default branch: `z8-c311`). It spans three clearly separated tracks built on the same Z8 `C3.11` (`04.06.p0`) foundation:

1. **Core Extension Runtime & `.wlm` Module SDK (`src/`, `host/`, `examples/`, `modules/`)**
   Adds native **Fenestra** and **Plugin Manager** (`插件管理`) menus inside the Z8 Setup Menu, an abnormal-shutdown boot guard, dynamic `.wlm` module loading from the memory card, an AArch64 C SDK for custom UI/OpenVG scenes, and a restricted `INFO` host debug bridge.
2. **Public Integrated Firmware Archive (`research/z8-live/`, `v36` through `V62` & `v71r7-lut-preview`)**
   Reproducible stock-to-`v36` reconstruction (`from-stock/build.py`), public incremental checkpoints through **`V62`** (`feat-onemov/`), and the **`V71.7-r7`** native Photo Shooting Menu LUT browser & plugin sources (`modules/` and `research/z8-live/build-tools/livediag/feat-onemov/v71r7-lut-preview/`).
3. **`V71.7-r7` All-in-One Feature Line (`Z8-v71.7r7-LutPreview-362ee2a5`, internal `04.06.p0`, Bootguard Build ID `0x8394568f`)**
   The latest integrated firmware & 5-plugin generation delivering:
   - **Real-Time 3D LUT Preview (EVF + LCD + 45.4MP JPEG + Live Photo)**: Native Nikon **Photo Shooting Menu row 47 (`Select LUT` / `选择 LUT`)** category & `.cube` browser + dedicated **`lut.wlm`** plugin, powered by a **528 KiB L2-resident pre-baked `64×64×33` YCbCr 3D LUT** (`0 B` heap growth, Class `0x24` frozen at `0xE8C000`), inline NEON plane copy, auto-restore across cold reboots, clean Movie-mode `pool_suspend` (`0 B` RAM), and `14-bit .NEF` RAW `0-diff` untouched.
   - **EXPEED 7 CEVA-XM6 Hardware Vector DSP Acceleration (`Slot 39/40/41`)**: Coexists bit-identically inside stock `z8_xm6_elf0.elf` (`51/51` `dcli.exe` PASS, `0-diff`) with zero-risk automatic ARM NEON fallback, on-device 4-case self-test (`XM6_DIAG.LOG`), and **`V71.7-r7` XM6 bridge de-contention** (splitting static readiness from up-to-16 ms mailbox-idle waiting with `dly_tsk(0)` handoff and hoisting IOMMU/D-Cache ops outside `xm6_busy`, eliminating the ~21% `ERR_NOT_READY` fallback measured on `V71.7-r6`).
   - **Reverse Single-Pass `Layer 0 → Live` Sharing + Post-Shutter 45MP Still Deferral**: Both EVF (`1280×852`) and LCD (`1024×682`) push pre-grain LUT+HDF NV12 frames directly into Live `LBUF` (`FS_TB_GRADED_READY`), eliminating duplicate CPU2 grading and lifting the LCD 30 fps ceiling; post-shutter 45.4MP still processing defers until Live post-roll finishes (hard-capped at `1.65 s`, bypassed when burst queue is full), eliminating post-shutter Live `.MOV` freezes.
   - **Clean 7-Row `Fenestra` Home + 5 Built-In Trilingual `.wlm` Plugins**: `filmsim.wlm`, `livephoto.wlm`, `lut.wlm`, `slotcard.wlm`, and `portrait.wlm` (EN / 简体中文 / 繁體中文 with `FontCJK-ZhUI` + live `1 s` language sync + native `Debug Settings` subpages), plus **C64 Native Cloud Icons (`[FC]C-10..C-64`)** and **Upstream NFP `v0.5.1` (`25fefa1`)**.

> [!IMPORTANT]
> **Read Before Building, Testing, or Deploying**
> - **No prebuilt firmware binaries, Nikon keys, or extracted `ZF_GRAIN.BIN` tables are distributed in this repository.** You must supply your own official Nikon Z8 `C3.11` firmware (`Z_8_0311.bin` or extracted `MAIN`) and any proprietary grain or `.cube` LUT assets.
> - **Understand each build track and archive boundary:**
>   - `host/build_z8_core.py` outputs a raw **`MAIN` image for QEMU emulation only** ([`z8-qemu`](https://github.com/YCX81/z8-qemu))—it is **not** a flashable camera update file, and `fenestra.py make-patch` (inherited for ZR/Z9) does **not** build Z8 packages. `host/build_sdk_module.py` builds standalone `.wlm` SDK modules without requiring stock firmware.
>   - `research/z8-live/from-stock/build.py` reconstructs `v36.main` and a complete `Z_8_0406.bin` container directly from stock `Z_8_0311.bin`.
>   - **[`research/z8-live/from-stock-v71r7/build.py`](research/z8-live/from-stock-v71r7/README.md)** reconstructs the complete **`V71.7-r7`** (`main.main` `362ee2a5...` & `Z_8_0406.bin` `eca451be...`) **bit-for-bit on a single Linux/WSL machine directly from official stock `Z_8_0311.bin`** (with zero private intermediate `.main` images; optional `--no-xm6` for a 100% pure-`clang` build and `--skip-fontzh` for English-only).
> - **Honest Verification Status (`physical_camera_verified = false` for `V71.7-r7`):**
>   - Prior `V71.1–V71.6` physical Nikon Z8 field profiling and USB/Live `.MOV` telemetry directly identified and drove the four `V71.7-r7` fluency root-fixes.
>   - **`V71.7-r7` (`362ee2a5`, Bootguard `0x8394568f`)** has passed all automated suites — **CEVA-XM6 `dcli.exe` 51/51 PASS**, **0-Q-Spill static audit (1,114 linked functions, 0 Q-spills)**, **Host ASan+UBSan (`lut_core` 8/8, `xm6_bridge` 9/9, `layer0` 9/9, `photo_menu` 6/6)**, **6 ARM64/Unicorn suites** (`FontCJK`, `Layer0` 63 checks, `XM6 Bridge` 124 checks, `FilmSim` 70 checks, `HangFix`, `5-WLM CJK` 0 missing glyphs), and **both whole-guest QEMU suites (`#19-r7` 14/14 PASS, `#20-r7` 13/13 PASS)** — and is explicitly marked **`physical_camera_verified = false`** pending physical Z8 flashing and field verification.

---

## UI & Feature Preview (`V71.7-r7`, 5 `.wlm` Plugins + Native Photo Menu 3D LUT + XM6 DSP)

| Clean 7-Row `Fenestra` Home Menu | `Plugin Manager` Listing All 5 `.wlm` Plugins |
| :---: | :---: |
| ![Fenestra Home Menu](docs/images/v717-fenestra-home-zh.png) | ![Plugin Manager 5 WLM Plugins](docs/images/v717-plugin-manager-5wlm-zh.png) |
| **Photo Shooting Menu Row 47 (`Select LUT` / `选择 LUT`)** | **Native 3D LUT Category Browser (`FENESTRA/LUTS/`)** |
| ![Photo Shooting Menu Row 47 Select LUT](docs/images/v717-lut-photo-menu-zh.png) | ![3D LUT Categories](docs/images/v717-lut-categories-zh.png) |
| **Paginated `.cube` File List in Photo Menu** | **Loaded 3D LUT Status (`Ready` / `已载入`)** |
| ![3D LUT File List](docs/images/v717-lut-list-zh.png) | ![3D LUT Loaded Status](docs/images/v717-lut-loaded-zh.png) |
| **Live View with Real-Time 3D LUT Applied** | **`3D LUT` Plugin (`lut.wlm`) Root Menu** |
| ![Live View with Real-Time 3D LUT](docs/images/v717-lut-liveview-active.png) | ![3D LUT Plugin Root Menu](docs/images/v717-lut-plugin-root-zh.png) |
| **`Film Simulation` (`filmsim.wlm`) Root Menu** | **`filmsim.wlm` Debug & XM6 DSP Subpage** |
| ![FilmSim Root Menu](docs/images/v717-filmsim-root-zh.png) | ![FilmSim Debug Subpage](docs/images/v717-filmsim-debug-zh.png) |
| **EXPEED 7 CEVA-XM6 Hardware DSP Self-Test Readout** | **`Live Photo` (`livephoto.wlm`) Root Menu** |
| ![XM6 Hardware DSP Diagnostic](docs/images/v717-xm6-diag-zh.png) | ![Live Photo Plugin Root Menu](docs/images/v717-livephoto-root-zh.png) |
| **C64 Native Cloud Icons (`Fn` Dial Strip `[FC]C-10..C-64`)** | **C64 Native Cloud Icons (`i`-Menu 12-Grid)** |
| ![C64 Fn Dial Strip Native Icons](docs/images/v715-c64-fn-strip-zh.png) | ![C64 i-Menu Native Icons](docs/images/v715-c64-imenu-zh.png) |
| **Dual-Card C64 `Slot card` (`slotcard.wlm`)** | **`Portrait Video` Runtime Gate (`portrait.wlm`)** |
| ![Slot Card Dual Card Plugin](docs/images/v71-slotcard-en.png) | ![Portrait Video Plugin](docs/images/v71-portrait-wlm-en.png) |

---

## V71.7-r7 Feature Architecture & Highlights

> For exhaustive menu-by-menu tables (every root row, every `Debug Settings` subpage row, exact EN/ZH-CN/ZH-TW labels, defaults, and diagnostic token decoders), see **[docs/features-and-usage.md](docs/features-and-usage.md)**.

### 1. Real-Time 3D LUT Preview (Native Photo Menu Row 47 + `lut.wlm`)

- **Native Nikon Photo Shooting Menu Integration (Row 47, `Select LUT` / `选择 LUT`)**:
  - Opens a native 23-row paginated submenu (`Page 0x5D6`) directly inside **Photo Shooting Menu**, supporting `Off` (`无LUT`), 20 items/page (`Prev` / `Next`), 1-level category subfolders under `FENESTRA/LUTS/<category>/*.cube` (with built-in localized names for `01-Fuji-Film` through `10-Creative-Vintage` plus any custom category folder), and flat `FENESTRA/*.cube` files under `Root LUTs` (`根目录 LUT`), up to `2 MiB` per `.cube` file.
  - Persisted atomically in dual-slot CRC32 **`FENESTRA/FXPHOTO0.CFG` / `FXPHOTO1.CFG`** for automatic cold-reboot restore; cleanly suspends its pool (`0 B` RAM) in Movie mode; leaves **14-bit `.NEF` RAW `0-diff` untouched**.
- **V71.7-r7 528 KiB L2-Resident Pre-Baked `64×64×33` YCbCr 3D LUT**:
  - Carved from the unused tail of `photo_workspace` (`stage_cpu + 0x80000`) — **`0 B` General heap growth**, Class `0x24` strictly frozen at `0xE8C000`.
  - Uses `2×2` Bayer chroma dithering + 3-bit Y linear interpolation (neutral-gray zero color cast, NEON == Scalar bit-exact `0-diff`), inline NEON plane copy, and fused per-row Grain D-Cache clean.
- **`lut.wlm` Plugin Controls (`Plugin Manager → 3D LUT`)**:
  - Root switches (`FXLUT0/1.CFG`, all default `On`): `3D LUT Enable`, `Active LUT` (read-only filename), `Live View Preview` (Layer 0 gate), `Still JPEG LUT` (Stage 0 Still gate), `Live Photo LUT` (Stage 0 Live gate), `Clear Active LUT`, and `Debug Settings` (`Pipeline & Accel`, `Layer 0 Preview`, `Health & Drive`, `Watchdog`, `Reset Preview Pool`).

### 2. EXPEED 7 CEVA-XM6 Hardware DSP Acceleration + `filmsim.wlm`

```text
  [14-bit .NEF RAW] ──────────────────────────────────────────► Written untouched (0-diff bit-identical)
  [Layer 0 Live View / JPEG YCbCr / Live NV12]
        │
        ▼ Stage 0: Real-Time 3D LUT (528 KiB L2 Baked 64×64×33 YCbCr / 1D-XM6 / 3D-NEON)
        ▼ Stage 1: Ricoh Optical HDF (CEVA-XM6 Slot 41 hardware DSP or ARM NEON fallback; 1/8, 1/4, 1/2)
        ▼ Stage 2: Nikon Zf C3.01 18-Level Silver-Halide Film Grain (CEVA-XM6 Slot 39/40 or ARM NEON; default Sharp)
        ▼
  [EVF / LCD Preview · 45.4MP JPEG · 1280×852 HEVC Live Photo]
```

- **CEVA-XM6 Coexistence & Zero-Risk Fallback**: Both Nikon Zf Grain (`Slot 39/40`) and Ricoh Optical HDF (`Slot 41`) coexist inside stock `z8_xm6_elf0.elf` (`.text.z8fx = 156,736 / 158,336 B`, `.rodata.z8fx = 2,016 / 2,048 B`, `51/51` `dcli.exe` PASS, `0-diff`). Any hardware fault or busy breaker seamlessly falls back to bit-exact ARM NEON.
- **V71.7-r7 XM6 Bridge De-Contention**: Splits static hardware-ready checking from locked mailbox-idle waiting (up to `16 ms` queuing + `dly_tsk(0)` waiter handoff) and hoists IOMMU mapping + pre-submit D-Cache clean/invalidate outside `xm6_busy`, eliminating the **~21–22.5% `ERR_NOT_READY` fallback** measured on `V71.7-r6`.
- **`filmsim.wlm` (16 Controls with `Debug Settings` Subpage)**:
  - Root controls: `Apply Mode` (`Global` / `Per-NP3`), `Grain Intensity` (`Off`, `1..6`), `Grain Size` (`1 Small` / `2 Medium` / `3 Large`), `Grain Texture` (`Soft (Zf)` / `Sharp`, default **`Sharp`**), `HDF Level` (`Off` / `Weak 1/8` / `Medium 1/4` / `Strong 1/2`), `Live Photo Link` (`Off` / `On`), `WYSIWYG Preview` (`Off` / `On`), `Bind Active NP3`, `Clear Active NP3`, and `Debug Settings`.
  - `Debug Settings` subpage: `XM6 DSP Mode` (`Auto` / `CPU Only`), `Run XM6 Self-Test` (4-case 0-diff hardware check + writes `FENESTRA/XM6_DIAG.LOG`), `Reset XM6 Breaker`, `Hardware & Runtime` (`EL1:ON/DM S:<n> L:<n> XM6:OK <ok>/<fb> [A|B]`), and `Reset Defaults`.
  - Per-NP3 recipe binding persists across 95 slots (`FLMREC0/1.CFG` + `SC*.REC +0x39C` `'Z8FX'`), automatically entering `0 B` dormant bypass on unbound Picture Controls.

### 3. Reverse Single-Pass `Layer 0 → Live` Sharing & Post-Shutter 45MP Deferral (`V71.7-r7`)

- **Reverse Single-Pass `Layer 0 → Live` Sharing (EVF `1280×852` & LCD `1024×682`)**:
  - When LUT / Layer 0 preview is active, Layer 0 pushes the already-graded + HDF NV12 frame (after Stage 1 HDF, before Stage 2 Grain) directly into the Live ring buffer (`LBUF`) as `FS_TB_GRADED_READY` at `>= 18 ms` cadence.
  - CPU0 skips duplicate scaling/submission, `live_worker` and XM6 avoid running a second redundant LUT + HDF pass, and LCD preview is no longer capped at 30 fps by legacy `Live → LCD` sharing. Adaptive 30 fps EVF pacing engages only when standalone Live runs without reverse sharing.
- **Post-Shutter 45.4MP Still Deferral**:
  - While `live_runtime_post_roll_active()` is true and the 2-slot burst queue is not full, 45.4MP still `PENDING → COPY` defers until Live post-roll finishes (hard-capped at **`1.65 s`**, well under the 30 s timeout), keeping post-shutter Live `.MOV` tail frames at a smooth `29.97 fps` instead of freezing.

### 4. All 5 Trilingual `.wlm` Plugins & Clean 7-Row `Fenestra` Home

| Plugin File (`FENESTRA/`) | Exact Size (`V71.7-r7`) | English / 简体中文 / 繁體中文 | Purpose & Subpage |
| :--- | :---: | :--- | :--- |
| **`filmsim.wlm`** | `32,540 B` | `Film Simulation` / `胶片模拟 / HDF` / `膠片模擬 / HDF` | Nikon Zf 18-level Grain, Ricoh Optical HDF, WYSIWYG Preview, Per-NP3 Binding + `Debug Settings` (XM6 DSP mode/self-test/breaker/HW status). |
| **`livephoto.wlm`** | `15,860 B` | `Live Photo` / `实况照片` / `實況照片` | Live Photo `On/Off`, Full-Duplex `Shutter Sound` (`0.0 ms` gap), `Playback Key` (`REC/Off`), live `Status` + `Debug Settings` (Capture/Wave5/Audio/PB/Flag stats & `Export LIVELOG.BIN`). |
| **`lut.wlm`** | `15,624 B` | `3D LUT` / `3D LUT 调色` / `3D LUT 調色` | Master `3D LUT Enable`, `Active LUT`, independent `Live View` / `Still JPEG` / `Live Photo` LUT switches, `Clear Active LUT` + `Debug Settings` (Pipeline/L0/Health/Watchdog/Reset Pool). |
| **`slotcard.wlm`** | `56,552 B` | `C64 Filter Slots` / `C64 滤镜槽` / `C64 濾鏡槽` | `Slot Limit` (`C64`/`C32`), strict dual-card `Storage Card` (`Auto`/`CFexpress (A)`/`SD (B)`), `[FC]C-10..C-64` native cloud icons, `Fn`+Dial C1–C64 + `Debug Settings` (`Slot State`, `Store Journal`, `Boot Card Latch`). |
| **`portrait.wlm`** | `7,348 B` | `Portrait Video` / `竖屏录像` / `直拍錄影` | Instant runtime `Portrait Video` (`On`/`Off`) gate for 8-hook `PortraitPB` (`tkhd` + `NCTG 0x0018`, vertical video OSD + histogram) + `Debug Settings` (`Runtime Status`, `Note`). |

- **`FontCJK-ZhUI` & Live Language Sync**: Hooks `set_primary_font` to chain stock ASCII `Font[18]` with `CJK_EXT_1` + `CJK_EXT_2` (`3,755/3,755` SC + `3,755/3,755` TC, `0` missing glyphs across all 5 plugins). Changing `Setup Menu → Language` updates all 5 plugins live via `SLOTS_LANG_GET` (op `15`) with no reboot required.
- **Upstream NFP `v0.5.1` (`25fefa1`) & Hardening**: Retains Open-Gate 4.1K 3:2 fixes, 10-bit SDR HEIF, Still Crops (`4:3 / 2.39:1 / 65:24`), EVF Pool #6 `0x001C2000` green-edge fix, playback freeze/bounce-back fix, and A2 non-fatal transient IPC hardening across all 5 `.wlm` plugins.

---

## Memory Card Layout & `V71.7-r7` Upgrade Guide

```text
<Card Root>/
├── Z_8_0406.bin                     # Firmware update container (ONLY during camera update; delete afterward)
├── FENESTRA/
│   ├── STARTUP-Z8.0                 # [64 B]     Fenestra boot & auto-activate config
│   ├── filmsim.wlm                  # [32,540 B] Zf Grain / Ricoh HDF / XM6 DSP / Per-NP3 plugin
│   ├── livephoto.wlm                # [15,860 B] Live Photo & Full-Duplex Audio plugin
│   ├── lut.wlm                      # [15,624 B] Real-Time 3D LUT control & debug plugin
│   ├── slotcard.wlm                 # [56,552 B] Dual-card C32/C64 Picture Control plugin
│   ├── portrait.wlm                 # [7,348 B]  Portrait Video runtime gate plugin
│   ├── ZF_GRAIN.BIN                 # [27,588 B] Nikon Zf C3.01 grain table (ZFG1+CRC32, user-supplied)
│   ├── *.cube                       # [Optional] Flat .cube LUTs (shown under "Root LUTs" in Photo Menu)
│   ├── LUTS/
│   │   ├── 01-Fuji-Film/*.cube      # [Optional] Categorized .cube 3D LUTs (<= 2 MiB each, 1-level folders)
│   │   └── <CustomCategory>/*.cube  # [Optional] Custom category folders browsable in Photo Menu Row 47
│   ├── FXPHOTO0.CFG / FXPHOTO1.CFG  # [Auto]     Dual-slot CRC32 active 3D LUT path persistence
│   ├── FXLUT0.CFG / FXLUT1.CFG      # [Auto]     Dual-slot CRC32 lut.wlm switches (32 B 'ZLT1')
│   ├── FLMSIM0.CFG / FLMSIM1.CFG    # [Auto]     Dual-slot CRC32 global FilmSim/HDF/XM6 config (64 B 'ZFS1')
│   ├── FLMREC0.CFG / FLMREC1.CFG    # [Auto]     95-slot Per-NP3 recipe binding table
│   ├── SLTCARD0.CFG / SLTCARD1.CFG  # [Auto]     Dual-slot CRC32 C64 drive selector (Auto/A/B)
│   ├── SCSEL0.CFG / SCSEL1.CFG      # [Auto]     C64 active slot persistence (Fn dial)
│   ├── SC*.REC                      # [Auto]     C10–C64 Picture Control archive (+0x39C 'Z8FX' binding)
│   ├── live-z8.0 / live-z8.1        # [Auto]     Dual-slot CRC32 Live Photo config (64 B 'FLP1')
│   ├── XM6_DIAG.LOG / LIVELOG.BIN   # [Auto]     On-demand diagnostic logs exported from Debug subpages
│   └── <name>.wlm.disabled          # [Optional] Disabled plugin (takes effect next boot)
├── DCIM/100NCZ_8/
│   └── DSC_0001.NEF / .JPG          # Original still photo (14-bit .NEF is ALWAYS 0-diff untouched)
├── LIVE/100NCZ_8/
│   └── DSC_0001.MOV                 # Paired 1280×852 HEVC + 48 kHz 24-bit stereo PCM Live Photo (~3 s)
└── LIVEDATA/100NCZ_8/
    └── DSC_0001.MAN / .LVP          # In-camera Live Photo pairing manifest & index
```

> [!TIP]
> **Upgrading Safely to `V71.7-r7` (`Z8-v71.7r7-LutPreview-362ee2a5`)**
> - **From `V71.7-r6`**: All 7 files inside `FENESTRA/` are byte-for-byte identical to `r6` — you only need to flash `Z_8_0406.bin` and delete it afterward.
> - **From `V71.7-r5` or earlier (`V71.5` / `V71.2` / `V62`)**: Replace all 7 shipping files inside `FENESTRA/` (`STARTUP-Z8.0`, `filmsim.wlm`, `livephoto.wlm`, `lut.wlm`, `slotcard.wlm`, `portrait.wlm`, and your `ZF_GRAIN.BIN`) **and** flash `Z_8_0406.bin`.
> - **Always preserve** `DCIM/`, `LIVE/`, `LIVEDATA/`, `FENESTRA/LUTS/`, `FXPHOTO*.CFG`, `FXLUT*.CFG`, `FLMSIM*.CFG`, `FLMREC*.CFG`, `SLTCARD*.CFG`, `SCSEL*.CFG`, `SC*.REC`, and `live-z8.*`.

---

## Build & Developer Guide (Core SDK, `v36` Reconstruction & `V62` Archive)

### 1. Prerequisites

- **Environment**: Linux or **Windows 11 WSL2 (`Ubuntu`)** is recommended for full firmware builds and companion [`z8-qemu`](https://github.com/YCX81/z8-qemu) emulation. (macOS and native Linux/Windows can also compile standalone `.wlm` SDK modules.)
- **Toolchains**:
  - Python `3.11+`
  - LLVM (`clang`, `ld.lld`, `llvm-objcopy`, `llvm-nm`) supporting `aarch64-none-elf`
  - `aarch64-linux-gnu-gcc` (with static linking support, used by `build_z8_core.py` for the `INFO` agent)

```bash
git clone --branch z8-c311 https://github.com/YCX81/fenestra-z8.git
cd fenestra-z8

python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r requirements.txt
# Optional: only needed if using USB/PTP host scripts
python -m pip install -r requirements-host.txt
```

### 2. Route A: Build `.wlm` SDK Modules & Core Emulation `MAIN`

```bash
# Minimal menu action example -> build/hello-z8/hello.wlm (no Nikon firmware required)
python host/build_sdk_module.py examples/hello/module.c \
  --id hello --target z8-c311 --output-dir build/hello-z8

# OpenVG animated character-rain demo -> build/demo-z8/demo.wlm
python host/build_sdk_module.py modules/demo/sdk.c \
  --id demo --target z8-c311 --output-dir build/demo-z8

# Optional v3 module with custom stack sizes (up to 1 MiB pkg / 4 MiB image) and SIMD context preservation
python host/build_sdk_module.py examples/hello/module.c \
  --id hello --target z8-c311 \
  --native-stack-bytes 131072 --user-stack-bytes 131072 --simd \
  --output-dir build/hello-v3-z8

# Build QEMU-only Z8 Core MAIN (input: 81,807,709 B stock Z8 C3.11 MAIN,
# SHA-256: 2b43a31c9de838f84b12416de4348a7004e65be07f853ec56ec2b10f3a8e3853)
python host/build_z8_core.py \
  --firmware /absolute/path/to/z8-c311-main.bin \
  --output-dir build/z8-core-local
```

> **Note:** `--output-dir` must not already exist. The generated `z8-core.main` is **strictly for QEMU emulation**, not for flashing to a camera.

### 3. Route B: Reconstruct Integrated Firmware (`v36`) from Stock `Z_8_0311.bin`

To reproducibly build the integrated Live Photo + C64 + Portrait + NFP baseline (`v36`) from unmodified official `Z_8_0311.bin` (`99,051,117` bytes, `SHA-256: d67413c2ede6b9ce8887175f20700bd9fbad2a973a4820968c45503d15d80d96`) with **Clang 18** + **Clang/LLVM 21**:

```bash
python -m pip install -r research/z8-live/from-stock/requirements.txt

python -B research/z8-live/from-stock/build.py \
  --stock /absolute/path/to/Z_8_0311.bin \
  --qemu-root /absolute/path/to/z8-qemu \
  --output-dir /absolute/path/to/v36-rebuilt
```

- See [research/z8-live/from-stock/README.md](research/z8-live/from-stock/README.md) and [RELEASE-v62.md](research/z8-live/build-tools/livediag/feat-onemov/RELEASE-v62.md).
- **Host `INFO` Bridge**: `FENESTRA_PTP_PROFILE=z8-c311` permits only the parameterless `info` query against SHA-256-pinned `fenestra-agent-z8-info` (arbitrary shell execution disabled). See [docs/debugging.md](docs/debugging.md).

### 4. Route C: NFP `v0.5.1`-Compatible Offline Builder (CLI Toolkit + macOS App `Z8 Patch Builder`)

`research/z8-live/nfp-builder/` (adapted from upstream [XinmiaoZhao/NikonFirmwarePatch](https://github.com/XinmiaoZhao/NikonFirmwarePatch) `v0.5.1` `25fefa1`, retaining `GPL-3.0-only`) provides full compatibility with both the upstream **CLI toolkit** and the **Apple Silicon macOS SwiftUI app (`Z8 Patch Builder`, macOS 14+)**, building and independently verifying firmware offline directly from your own official `Z_8_0311.bin` (`99,051,117 B`, SHA-256 `d67413c2...`) with no WSL or QEMU required:

- **100% Byte-Identical Upstream Compatibility**: All 4 upstream NFP `v0.5.1` groups (`video-recording`, `log-colour`, `still-photo-tools`, `raw-precapture`) and their 20 reviewed source-matched compilation units remain intact—all 15 upstream selections produce **byte-identical** output to official `NikonFirmwarePatch v0.5.1`.
- **5th Feature Group `live-filmsim` (`V71.7-r7` All-in-One)**: Applies the verified `V71.7-r7` overlay (including relocated 4-group NFP `v0.5.1` strictly below `0x40B50000`, Live Photo 30fps, Real-Time 3D LUT Layer 0 Preview, CEVA-XM6 HDF, PortraitPB, FontCJK-ZhUI, Dual-Card SlotCard, C64 i-Menu, and Fenestra Core), supports arbitrary `--version NN.NN --tag tD` (restamping all 4 Nikon identity sites, `0x401FB136`, and Bootguard `0x401FE060`), and exports the 6 open-source `FENESTRA/` runtime files (`STARTUP-Z8.0` + 5 `.wlm` plugins; supply your own `ZF_GRAIN.BIN` on the card if using Zf grain).
- **macOS App (`NikonFirmwarePatch-v71.7-r7-macOS.dmg`)**: Bundles the same toolkit, trimmed CPython 3.12.16, and `capstone 5.0.7`, adds the `live-filmsim` card (with a 4-step interactive guide animation and EN / 简体中文 / 繁體中文 UI), and runs the full 4-step pipeline (`patch.py --check` → `patch.py` → independent `verify.py` → read-back `.bin` + `FENESTRA/` SHA-256 verification).

```bash
# Option A: Download NikonFirmwarePatch-v71.7-r7-macOS.dmg (macOS 14+ Apple silicon)
#           or extract NikonFirmwarePatch-v71.7-r7-toolkit.zip from GitHub Release v71.7-r7:
python3 -m venv .venv && source .venv/bin/activate
python -m pip install -r requirements.txt
python patch.py Z_8_0311.bin --check

# Upstream NFP v0.5.1 only (byte-identical to official v0.5.1)
python patch.py Z_8_0311.bin --features video-recording log-colour still-photo-tools raw-precapture \
  --version 04.08 --tag p0 --output build/nfp-all

# Full V71.7-r7 (includes relocated NFP v0.5.1 + Live/FilmSim/LUT/XM6 + FENESTRA/ export)
python patch.py Z_8_0311.bin --features live-filmsim --version 04.06 --tag p0 --output build/v71r7

# Option B: Build the v71.7-r7 toolkit ZIP and macOS DMG from source (Apple clang 21.0.0 + Swift 6.3+)
cd research/z8-live/nfp-builder
python3 -B tools/build_release.py --tag v71.7-r7 --output /tmp/z8-toolkit
python3 -B macos/scripts/build_app.py \
  --toolkit-zip /tmp/z8-toolkit/NikonFirmwarePatch-v71.7-r7-toolkit.zip \
  --output /tmp/z8-app
```

---

## Verification Matrix, Precautions & Limitations

### `V71.7-r7` (`362ee2a5`, `0x8394568f`) Verification Receipts

| Verification Suite | Result | Verified Scope |
| :--- | :---: | :--- |
| **CEVA-XM6 `dcli.exe` Cycle-Accurate Sim** | **51/51 PASS** | GRAIN `25/25` + HDF `26/26` `0-diff` bit-exact (`.text.z8fx = 156,736 / 158,336 B`, `.rodata.z8fx = 2,016 / 2,048 B`). |
| **0-Q-Spill Static Audit** | **PASS (`0` spills)** | `1,114` linked functions audited, `q_spills = 0`. |
| **Host ASan + UBSan Suites** | **ALL PASS** | `lut_core` 8/8 (528 KiB baked LUT NEON==Scalar `0-diff`/PSNR/SMP/NEON copy), `xm6_bridge` 9/9 (pre-lock wait 0 fallback, reverse Layer 0→Live, post-shutter deferral, fused grain cache clean), `layer0` 9/9, `photo_menu` 6/6. |
| **ARM64 / Unicorn Suites (6 Suites)** | **ALL PASS** | Suite #5 FontCJK (`3,755/3,755` SC + TC + ops 13..16), Suite #14 Layer0 (`63` checks), XM6 Bridge (`124` checks, `0` forbidden hits), Suite #4 FilmSim (`70` checks), Suite #2 HangFix, and 5-WLM CJK glyph audit (`0` missing SC/TC). |
| **Whole-Guest QEMU `#19-r7`** | **14/14 PASS** | Clean 7-row Fenestra + 5 `.wlm` plugins + XM6 self-test/persistence/fallback (`z8_unk = 0`, `panic = 0`). |
| **Whole-Guest QEMU `#20-r7`** | **13/13 PASS** | Photo Menu Row 47 streaming `.cube` LUT + Layer 0 528 KiB baked LUT + Still + Movie `pool_suspend` (`0 B`) + reboot restore/clear (`z8_unk = 0`, `panic = 0`). |
| **NFP `v0.5.1` Compat CLI + macOS App (`v71.7-r7`)** | **17/17 PASS** | `validate_gui_catalog`, `sync_strings`, and `Z8 Patch Builder.app --self-test` PASS; all 15 upstream selections App↔CLI byte-identical, `live-filmsim` (+ `FENESTRA/`) App↔CLI byte-identical, source-vs-prebuilt exact match, and all 4 negative controls PASS. |
| **Physical Nikon Z8 Hardware** | **`physical_camera_verified = false`** | Prior `V71.1–V71.6` physical Z8 profiling drove the four `r7` fluency fixes; **`V71.7-r7` (`362ee2a5`) awaits physical Z8 flash & field verification** (EVF/LCD fps, XM6 fallback rate, and post-shutter Live `.MOV` cadence). |

### Key Precautions

1. **Never confuse `.main` with `.bin`**: Raw `.main` files (`z8-core.main`, `v36.main`) are unwrapped images for QEMU or linking—never copy a `.main` onto a memory card expecting a firmware update.
2. **Wait for Live saving between rapid shots**: Firing another still while `◎LIVE` is in its `Busy` sweep saves the new still normally, without a paired Live clip.
3. **Unloading `.wlm` plugins requires a reboot**: Disabling a `.wlm` in `Setup → Fenestra → Modules` (`已加载插件`) renames it to `.wlm.disabled` for the **next** boot; never enable two copies of the same module ID simultaneously.
4. **Source hygiene**: Never commit Nikon firmware, keys, `ZF_GRAIN.BIN`, or personal media; run `python -B verify_source.py` to verify the tracked source inventory.

---

## Documentation & License

- **V71.7-r7 Complete User & Menu Reference**: **[Detailed V71.7-r7 Feature & Menu Reference (EN)](docs/features-and-usage.md)** · [V71.7-r7 详细功能与全菜单设置手册 (简体中文)](docs/features-and-usage.zh-CN.md)
- **Core & SDK Docs**: [Getting Started](docs/getting-started.md) · [Architecture](docs/architecture.md) · [SDK API Reference](docs/api-reference.md) · [Module Development](docs/module-development.md) · [Module Format](docs/module-format.md) · [Debugging & PTP](docs/debugging.md) · [Porting Guide](docs/porting.md)
- **Z8 Live Archive**: [Live Source Map](research/z8-live/README.md) · [Stock-to-v36 Reconstruction](research/z8-live/from-stock/README.md) · [V62 Release Notes](research/z8-live/build-tools/livediag/feat-onemov/RELEASE-v62.md) · [Core Port Notes](port-z8/PORT-STATUS.md)
- **Companion Emulator**: [`YCX81/z8-qemu`](https://github.com/YCX81/z8-qemu) (Nikon Z8 `C3.11` QEMU `10.2.4` machine model & whole-guest `V71.7-r7` verification harness)
- **License**: [CC BY-NC-SA 4.0](LICENSE) with the [Commercial Photography Exception](EXCEPTIONS.md) and [Z8 Adaptation Terms](LICENSE-Z8.md). Using the software for paid photography and filmmaking is explicitly permitted; selling covered software, modules, or paid flashing services is **not** permitted. Third-party components retain their respective licenses.
