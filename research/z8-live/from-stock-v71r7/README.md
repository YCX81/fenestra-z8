# Reconstructing `V71.7-r7` Directly from Official Nikon Z8 `C3.11` (`Z_8_0311.bin`)

`build.py` in this directory is a **single-machine, clean-room reconstruction entry point** for **`V71.7-r7`** (`v71r7-lut-preview`, internal `04.06.p0`, Bootguard Build ID `0x8394568f`).

Starting **only** from the user's own official Nikon Z8 `C3.11` update file (`Z_8_0311.bin`, SHA-256 `d67413c2ede6b9ce8887175f20700bd9fbad2a973a4820968c45503d15d80d96`) plus the public `fenestra-z8` and `z8-qemu` repositories, it executes Stages 0 through 4 locally with **zero private intermediate `.main` images** and **zero remote SSH hosts**.

## Prerequisites (Linux or Windows WSL2)

- **Official Nikon Z8 `C3.11` firmware**: `Z_8_0311.bin` (`99,051,117 B`, supplied by the user — never distributed in this repository).
- **Companion repository**: [`YCX81/z8-qemu`](https://github.com/YCX81/z8-qemu) (`main` branch).
- **Compilers & LLVM tools**: `clang-18` (for historical Fenestra Core), `clang-21`, `ld.lld`, `llvm-nm`, `llvm-objcopy`, `llvm-objdump`.
- **Python 3.11+**: `fonttools` (only required for Stage 3 `FontCJK-ZhUI`; skip with `--skip-fontzh` if building English-only).
- **CJK TrueType font (for Stage 3 `FontCJK-ZhUI`)**: On Windows WSL2, `/mnt/c/Windows/Fonts/msyh.ttc` is auto-detected. On native Linux, pass `--font /path/to/NotoSansCJK-Regular.ttc` (or any quadratic/TrueType CJK font), or use `--skip-fontzh`.

## Quick Start

```bash
python3 research/z8-live/from-stock-v71r7/build.py \
  --stock /path/to/Z_8_0311.bin \
  --qemu-root /path/to/z8-qemu \
  --output-dir /path/to/out_v71r7
```

### Useful Options

| Flag | Description |
| :--- | :--- |
| `--font <path>` | Explicit `.ttc`/`.ttf` font path for Stage 3 `FontCJK-ZhUI` (reproduces exact `V71.7-r7` hashes when using `msyh.ttc`). |
| `--skip-fontzh` | Skip Stage 3 `FontCJK-ZhUI` (no `fonttools` or CJK font required; produces English UI image). |
| `--no-xm6` | Skip the EXPEED 7 CEVA-XM6 DSP overlay for a **100% `clang`-only** build (all HDF & Grain paths run cleanly on ARM NEON). |
| `--main-only` | Emit `main.main` only (for `z8-qemu` testing) and skip `Z_8_0406.bin` update container packing. |

## Bundled Open-Source / Third-Party Patch Assets (`assets/`)

Zero Nikon firmware bytes, zero Nikon keys, and zero `ZF_GRAIN.BIN` tables are included:

- `assets/cardslot_v71r4/cardslot.c` — compiled live by `clang-21` during Stage 0.
- `assets/legacy_steps/*.py` & `assets/portrait_pb/*.py` — pure Python / inline ARM64 assembly overlays applied live during Stage 0 & Stage 2.
- `assets/nfp_v050/` — GPL-3.0-only relocated patch bundle for upstream `NikonFirmwarePatch v0.5.1` (`25fefa1`), accompanied by its reproducible builder `build_nfp_v050_bundle.py`.
- `assets/xm6_elf0/coexist_nikondma/` — 100% self-authored CEVA-XM6 Core 0 DSP kernels (corresponding C source in `../build-tools/livediag/feat-onemov/v71r7-lut-preview/work/xm6_coexist_src/`). Pre-compiled with CEVA-ToolBox V18.0.3 because no open-source CEVA-XM6 compiler exists; omit anytime via `--no-xm6`.

## Pinned `V71.7-r7` Reference SHA-256 Hashes (Full Build with `msyh.ttc` + XM6)

- **Stage 0 (`stage0-base.main`)**: `ffed106403d0e7367a045b3ad3b99512b17f8cb1bffd010398c4fa2df823f398`
- **Final `main.main`**: `362ee2a5491859a762b35ed8459cf22c212ae5babb38915daa15e3b801533c8a` (`build_id 0x8394568f`)
- **Final `Z_8_0406.bin`**: `eca451be7160945c098e2641f9daf11b9c6755aa4b8ab5c30bdcdd228a8ecfe4`
