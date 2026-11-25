/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PS2 unity build (PS7: build_ps7.py): one relocatable object for the linker; the menu action
   (section .text.entry, firmware.c) is placed first. */
#define PSM_FIRMWARE 1
#include "firmware.c"
#include "job.c"
#include "io_pipe.c"
#include "browser.c"
#include "thumbnail.c"
#include "group.c"
#include "nefx.c"
#include "nef.c"
#include "psm.c"
#include "af_window.c"
/* Read-only ABI facts for bounded stale-state fixtures and saved diagnostics. */
__attribute__((used)) static const uint32_t ps9_runtime_layout[]={
    sizeof(psm_runtime),__builtin_offsetof(psm_runtime,status_nodes),
    __builtin_offsetof(psm_runtime,status_parent),sizeof(card_job),sizeof(psm_scan),sizeof(nefx),
    /* PS11: fields of the power-off-during-merge fixture (make_language_fixture.py --stale-merge) */
    __builtin_offsetof(psm_runtime,modal),__builtin_offsetof(psm_runtime,operation),
    __builtin_offsetof(psm_runtime,modal_serial),__builtin_offsetof(psm_runtime,review_serial)
};
