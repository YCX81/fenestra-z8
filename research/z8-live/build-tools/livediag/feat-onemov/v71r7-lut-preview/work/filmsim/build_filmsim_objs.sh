#!/usr/bin/env bash
# Standalone freestanding AArch64 builder for the 15 V71.6 FilmSim EL1 units
# (Nikon Zf Film Grain + Ricoh Optical HDF + CEVA-XM6 Core 0 Bridge; Fuji stripped).
# Compiles 13 non-kernel units strictly scalar (-mgeneral-regs-only) and ONLY
# the 2 CPU2 worker kernel units with AArch64 NEON (0 FP instructions anywhere,
# 0 .data/.bss).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${FILMSIM_SRC:-$SCRIPT_DIR/src}"
SRC_FIX37="${SRC_FIX37_DIR:-$SCRIPT_DIR/../src-fix37}"
LUT_CORE_DIR="${LUT_CORE_DIR:-$SCRIPT_DIR/../lut_core}"
LUT_PREVIEW_DIR="${LUT_PREVIEW_DIR:-$SCRIPT_DIR/../lut_preview}"
LUT_MENU_DIR="${LUT_MENU_DIR:-$SCRIPT_DIR/../lut_menu}"
FENESTRA_INC="${FENESTRA_INC_DIR:-/Users/ycx/Nikon/fenestra-z8/include}"
OUT="${OUT_DIR:-$SCRIPT_DIR/build/objects}"
CC="${CC:-clang-21}"

mkdir -p "$OUT"

COMMON_FLAGS=(
  --target=aarch64-none-elf
  -Os
  -std=c11
  -ffreestanding
  -fno-builtin
  -fno-stack-protector
  -fno-unwind-tables
  -fno-asynchronous-unwind-tables
  -ffunction-sections
  -fdata-sections
  -mstrict-align
  -mno-outline-atomics
  -Wall
  -Wextra
  -Werror
  -I"$SRC_FIX37"
  -I"$SRC_FIX37/ui/core/include"
  -I"$SRC_FIX37/codec/freestanding"
  -I"$FENESTRA_INC"
  -I"$LUT_CORE_DIR"
  -I"$LUT_PREVIEW_DIR"
  -I"$LUT_MENU_DIR"
  -I"$SRC/include"
  -I"$SRC/hdf"
  -I"$SRC/zf_grain"
  -I"$SRC/pipeline"
  -I"$SRC/photo"
  -I"$SRC/runtime"
  -I"$SRC/xm6"
)

SCALAR_FLAGS=(
  "${COMMON_FLAGS[@]}"
  -mgeneral-regs-only
  -DHDF_SCALAR
  -DZF_GRAIN_SCALAR
)

# Exact 15 FilmSim EL1 compilation units (13 scalar + 2 selective-NEON).
# Prefix "filmsim_" preserves existing slot numbering for surviving objects
# (filmsim_02-z8_hdf_kernels, filmsim_07-z8_zf_grain_kernels, filmsim_18-photo_hooks)
# and deterministic linker input matching in filmsim_section.ld.inc.
SCALAR_UNITS=(
  "01:hdf/z8_hdf.c"
  "06:zf_grain/z8_zf_grain.c"
  "08:pipeline/z8_filmsim_pipeline.c"
  "09:pipeline/z8_filmsim_stage_hdf.c"
  "11:pipeline/z8_filmsim_stage_grain.c"
  "12:pipeline/z8_filmsim_assets.c"
  "13:photo/z8_photo_source.c"
  "14:runtime/filmsim_runtime.c"
  "15:photo/z8_native_filmsim_photo.c"
  "16:photo/z8_native_photo_platform.c"
  "17:photo/photo_hooks.c"
  "18:photo/photo_hooks.S"
  "20:xm6/z8_xm6_bridge.c"
)

NEON_UNITS=(
  "02:hdf/z8_hdf_kernels.c"
  "07:zf_grain/z8_zf_grain_kernels.c"
)

for entry in "${SCALAR_UNITS[@]}"; do
  idx="${entry%%:*}"
  rel="${entry#*:}"
  base="$(basename "$rel")"
  "$CC" "${SCALAR_FLAGS[@]}" -c "$SRC/$rel" -o "$OUT/filmsim_${idx}-${base}.o"
done

for entry in "${NEON_UNITS[@]}"; do
  idx="${entry%%:*}"
  rel="${entry#*:}"
  base="$(basename "$rel")"
  "$CC" "${COMMON_FLAGS[@]}" -c "$SRC/$rel" -o "$OUT/filmsim_${idx}-${base}.o"
done

echo "Built 15 FilmSim EL1 objects in $OUT (13 scalar + 2 selective-NEON)"
