#!/usr/bin/env python3
"""v11 Live changes on top of v10 (hardware 11-07, MAIN 6ac3526c):
  1. Screen/Live-icon flicker came from the periodic icon refresh (icon_refresh_ms 500 hid and
     re-attached the overlay every 0.5 s; setting it to 0 over USB stopped the flicker).
     -> default icon_refresh_ms 0; a refresh, when enabled, re-presents in place (no hide/detach).
  2. Handheld clips judder: 100 of ~1500 frames were dropped as seq_changed (a newer frame arrived
     during the copy). With picker 2/3 the copied buffer is a finished predecessor and the tear
     check re-reads it after the copy, so such a frame is kept when the tear check is clean.
  3. READY (green icon) now needs an encoded frame within 1 s instead of 250 ms, so a few dropped
     frames no longer flip the icon LIVE <-> LIVE... (each flip redraws the overlay).
Usage: v11_patch.py SRC_DIR (a copy of src-fix10)"""
import sys
from pathlib import Path
src = Path(sys.argv[1])


def sub(path, old, new, count=1):
    p = src / path; t = p.read_text()
    assert t.count(old) == count, (path, old[:80], t.count(old))
    p.write_text(t.replace(old, new))


C = 'capture/live_stream.c'
sub(C, ',1,250000u,1,1,1,1,3,500u};', ',1,1000000u,1,1,1,1,3,0u};')
sub(C, '    else if(now_seq!=seq){valid=0;D->seq_changed++;}',
    '    else if(now_seq!=seq && s->tune.picker>=2 && s->fr && s->fr_rows && !(fr.torn_y|fr.torn_uv) &&\n'
    '            LOAD(&c->cancel_epoch)==source.lease){D->accepted_late++;fr.flags|=2;}\n'
    '    else if(now_seq!=seq){valid=0;D->seq_changed++;}')
U = 'ui/live_ui.c'
sub(U, '    if(refresh&&status==u->display_status)u->refreshes++;\n',
    '    if(refresh&&status==u->display_status){\n'
    '        /* Same commands: re-present in place. Hiding first detached the overlay and made the\n'
    '         * whole screen flicker on every refresh (hardware 11-07). */\n'
    '        u->refreshes++;\n'
    '        if(!atomic_load(&u->display.readers)&&fx_native_display_present(&u->display,&u->scene)==FX_OK){u->presents++;u->last_present_us=t;return;}\n'
    '        u->present_fail++;u->last_present_us=t;\n'
    '    }\n')

# 4. Standby while USB is connected (hardware 11-07): the camera sends BSShutdown (event 7) after the
#    standby timer but does not power MAIN down while a computer is attached. Live (u->shutdown,
#    live_runtime_event(7)) and Fenestra (startup.shutdown -> stop modules, PTP off) both shut down
#    for good, so USB Debug went grey and Live stopped until a restart. Track UsbInsert(33)/
#    UsbRemove(34) - the camera reports the state at every boot - and while connected forward
#    event 7 straight to the stock handler Fenestra itself forwards to (0x401d6000), skipping both
#    shutdowns. If MAIN is powered down anyway, the next boot sees a same-build flag and the
#    boot guard allows one retry.
sub(U, '    uint64_t last_present_us;uint32_t presents,refreshes,present_fail,not_current;\n} UiState;',
    '    uint64_t last_present_us;uint32_t presents,refreshes,present_fail,not_current;\n'
    '    _Atomic uint32_t usb,usb_events,standby_kept;\n} UiState;')
sub(U, """    UiState *u=ui();
    if(u&&(event==7u||event==24u)) {""",
"""    UiState *u=ui();
    if(u&&(event==33u||event==34u)){atomic_store(&u->usb,event==33u);atomic_fetch_add(&u->usb_events,1u);}
    if(u&&event==7u&&atomic_load(&u->usb)) {
        atomic_fetch_add(&u->standby_kept,1u);
        return ((uint64_t (*)(void *,uint32_t))(uintptr_t)UINT64_C(0x401d6000))(message,sync);
    }
    if(u&&(event==7u||event==24u)) {""")
print('v11 Live patch applied to', src)
