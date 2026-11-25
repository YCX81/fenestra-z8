/* SPDX-License-Identifier: MIT
 * Fenestra EL0 SDK Module: 3D LUT (lut.wlm).
 */
#include "lut_core.h"

static LutModuleState g_lut_state;

int fenestra_module_entry(const FxModuleServices *base) {
    return lut_module_step(&g_lut_state, base);
}
