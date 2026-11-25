/* SPDX-License-Identifier: MIT
 * Fenestra EL0 SDK Module: Live Photo (livephoto.wlm).
 */
#include "livephoto_core.h"

static LpModuleState g_lp_state;

int fenestra_module_entry(const FxModuleServices *base) {
    return lp_module_step(&g_lp_state, base);
}
