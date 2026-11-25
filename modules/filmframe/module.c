/* SPDX-License-Identifier: MIT
 * Fenestra EL0 SDK Module: Film Frame / XPan (filmframe.wlm).
 */
#include "filmframe_core.h"

static FfModuleState g_ff_state;

int fenestra_module_entry(const FxModuleServices *base) {
    return ff_module_step(&g_ff_state, base);
}
