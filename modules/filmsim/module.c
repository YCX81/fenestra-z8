/* SPDX-License-Identifier: MIT
 * Fenestra EL0 SDK Module: Film Simulation (filmsim.wlm).
 */
#include "filmsim_core.h"

static FsModuleState g_fs_state;

int fenestra_module_entry(const FxModuleServices *base) {
    return fs_module_step(&g_fs_state, base);
}
