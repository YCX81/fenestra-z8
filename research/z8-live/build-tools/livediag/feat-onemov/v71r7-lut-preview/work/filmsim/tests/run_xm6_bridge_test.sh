#!/usr/bin/env bash
# Build & run Host ASan + UBSan acceptance tests for V71.6 EL1 ARM64 <-> CEVA-XM6 bridge
# against both ARM NEON (default) and Scalar reference kernel builds.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FILMSIM_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
V71R6_ROOT="$(cd "${FILMSIM_DIR}/../.." && pwd)"
FEAT_ROOT="$(cd "${V71R6_ROOT}/.." && pwd)"

DSP_KIT="${FEAT_ROOT}/v71r5-xm6-dsp-kit"
HDF_KIT="${FEAT_ROOT}/v71r5-xm6-hdf-kit"
FIX37_DIR="${V71R6_ROOT}/work/src-fix37"
FENESTRA_INC="${FENESTRA_INC_DIR:-/Users/ycx/Nikon/fenestra-z8/include}"

CC="${CC:-clang}"
TMP_DIR="$(mktemp -d /tmp/v71r6_xm6_bridge_test.XXXXXX)"
trap 'rm -rf "${TMP_DIR}"' EXIT

COMMON_CFLAGS=(
    -std=c11 -O2 -g
    -Wall -Wextra -Werror
    -fsanitize=address,undefined -fno-omit-frame-pointer
    -DFILMSIM_HOST_TEST=1
    "-I${FILMSIM_DIR}/src/include"
    "-I${FILMSIM_DIR}/src/xm6"
    "-I${FILMSIM_DIR}/src/runtime"
    "-I${FILMSIM_DIR}/src/photo"
    "-I${FILMSIM_DIR}/src/pipeline"
    "-I${FILMSIM_DIR}/src/hdf"
    "-I${FILMSIM_DIR}/src/zf_grain"
    "-I${FIX37_DIR}"
    "-I${FIX37_DIR}/ui/core/include"
    "-I${FENESTRA_INC}"
    "-I${DSP_KIT}/sim_harness"
    "-I${V71R6_ROOT}/work/xm6_coexist_src"
)

SRCS=(
    "${SCRIPT_DIR}/test_xm6_bridge.c"
    "${FILMSIM_DIR}/src/xm6/z8_xm6_bridge.c"
    "${FILMSIM_DIR}/src/photo/z8_native_filmsim_photo.c"
    "${FILMSIM_DIR}/src/photo/z8_photo_source.c"
    "${FILMSIM_DIR}/src/pipeline/z8_filmsim_pipeline.c"
    "${FILMSIM_DIR}/src/pipeline/z8_filmsim_stage_hdf.c"
    "${FILMSIM_DIR}/src/pipeline/z8_filmsim_stage_grain.c"
    "${FILMSIM_DIR}/src/pipeline/z8_filmsim_assets.c"
    "${FILMSIM_DIR}/src/hdf/z8_hdf.c"
    "${FILMSIM_DIR}/src/hdf/z8_hdf_kernels.c"
    "${FILMSIM_DIR}/src/zf_grain/z8_zf_grain.c"
    "${FILMSIM_DIR}/src/zf_grain/z8_zf_grain_kernels.c"
    "${DSP_KIT}/sim_harness/zf_grain_xm6_ref.c"
    "${V71R6_ROOT}/work/xm6_coexist_src/ricoh_hdf_xm6_v2_host.c"
)

echo "=== [1/2] Building & running test_xm6_bridge (NEON + ASan/UBSan) ==="
"${CC}" "${COMMON_CFLAGS[@]}" "${SRCS[@]}" -o "${TMP_DIR}/test_xm6_bridge_neon"
"${TMP_DIR}/test_xm6_bridge_neon"

echo "=== [2/2] Building & running test_xm6_bridge (SCALAR + ASan/UBSan) ==="
"${CC}" "${COMMON_CFLAGS[@]}" -DHDF_SCALAR=1 -DZF_GRAIN_SCALAR=1 "${SRCS[@]}" -o "${TMP_DIR}/test_xm6_bridge_scalar"
"${TMP_DIR}/test_xm6_bridge_scalar"

echo "=== ALL XM6 BRIDGE HOST ACCEPTANCE TESTS PASSED (NEON + SCALAR) ==="
