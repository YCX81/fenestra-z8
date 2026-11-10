"""Decode struct live_diag (352 bytes, magic 'LDG1') of the Live diagnostic build. Offset in live_runtime: 13064."""
import struct
DIAG_OFFSET=13064;DIAG_SIZE=352
DIAG_SIZE_V2=352+64+8+24
DIAG_SIZE_V3=DIAG_SIZE_V2+32
DIAG_SIZE_V4=DIAG_SIZE_V3+8
U64=['steps','ineligible','new_seq','stale','interval','lease_fail','geometry','valid1_fail','convert_fail','valid2_fail',
 'seq_changed','cancelled','busy','ok','enc_error','last_age_us','max_age_us','last_lease_us','max_lease_us','last_convert_us',
 'max_convert_us','sum_convert_us','n_convert','last_frames_during_copy','max_frames_during_copy','y','uv']
U32=['width','height','y_stride','uv_stride']
U64B=['loops','last_loop_us','max_loop_us','last_ui_us','max_ui_us','last_step_us','max_step_us','last_audio_us','max_audio_us',
 'last_sleep_us','max_sleep_us','loop_t0','snap_y_after','snap_uv_after']
def decode(b):
    d={'magic':hex(struct.unpack_from('<I',b,0)[0]),'version':struct.unpack_from('<I',b,4)[0]};o=8
    for k in U64:d[k]=struct.unpack_from('<Q',b,o)[0];o+=8
    for k in U32:d[k]=struct.unpack_from('<I',b,o)[0];o+=4
    for k in U64B:d[k]=struct.unpack_from('<Q',b,o)[0];o+=8
    assert o==DIAG_SIZE,o
    if len(b)>=DIAG_SIZE_V2:
        d['bufs']=[hex(x) for x in struct.unpack_from('<8Q',b,o) if x];o+=64
        d['nbufs'],d['version2']=struct.unpack_from('<II',b,o);o+=8
        d['accepted_late'],d['fast_rows'],d['slow_rows']=struct.unpack_from('<3Q',b,o);o+=24
    if len(b)>=DIAG_SIZE_V3:
        d['alloc_retry'],d['short_start'],d['geometry_skip'],d['geometry_since']=struct.unpack_from('<4Q',b,o);o+=32
    if len(b)>=DIAG_SIZE_V4:
        d['soft_cancel']=struct.unpack_from('<Q',b,o)[0]
    for k in ('y','uv','snap_y_after','snap_uv_after'):d[k]=hex(d[k])
    if d['n_convert']:d['avg_convert_us']=d['sum_convert_us']//d['n_convert']
    return d
