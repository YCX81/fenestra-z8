#!/usr/bin/env python3
"""v10 on top of v9 (flight_patch): hardware results 11-07 (MAIN 7e514951):
  picker 0 tore 26% of kept frames, picker 2 0.3%, picker 3 0/1316 -> default picker 3.
  1. tune.picker default 3 (still switchable over USB).
  2. stream.fr_drop (default 1): a frame whose tear check differs is dropped (result 14 torn_drop).
  3. stream.dcim_copy (default 1): after a successful Live write, copy LIVE/<f>/DSC_nnnn.MOV to
     DCIM/<f>/LIV_nnnn.MOV so the stock Nikon USB/MTP transfer (DCIM only, ~27 MB/s) can fetch it.
     Uses the Live file ops (exclusive create; removed on failure); EV 22 dcim_copy logs the result.
Usage: v10_patch.py SRC_DIR (a copy of src-fix9)"""
import sys
from pathlib import Path
src = Path(sys.argv[1])


def sub(path, old, new, count=1):
    p = src / path; t = p.read_text()
    assert t.count(old) == count, (path, old[:80], t.count(old))
    p.write_text(t.replace(old, new))


H = 'capture/live_stream.h'
sub(H, ' uint32_t fr_magic,fr_rows;uint32_t fr_head,fr_alloc_fail;struct live_fr *fr;void *fr_desc;uint64_t fr_torn,fr_checked;\n};',
    ' uint32_t fr_magic,fr_rows;uint32_t fr_head,fr_alloc_fail;struct live_fr *fr;void *fr_desc;uint64_t fr_torn,fr_checked;\n'
    ' uint32_t fr_drop,fr_dropped,dcim_copy,dcim_result;uint64_t dcim_copies;\n};')
sub(H, 'EV_FREEZE,EV_FREEZE_FAIL,EV_MUX_BEGIN,EV_MUX_END,EV_WRITER,EV_READY,EV_ENC_ERR,EV_MUX_ABORT,EV_NOTREADY,EV_ICON,EV_MUX_STAGE};',
    'EV_FREEZE,EV_FREEZE_FAIL,EV_MUX_BEGIN,EV_MUX_END,EV_WRITER,EV_READY,EV_ENC_ERR,EV_MUX_ABORT,EV_NOTREADY,EV_ICON,EV_MUX_STAGE,EV_DCIM};')

C = 'capture/live_stream.c'
sub(C, ',1,1,1,1,0,500u};', ',1,1,1,1,3,500u};')
sub(C, 's->fr_magic=0x31524646u;s->fr_rows=16;', 's->fr_magic=0x31524646u;s->fr_rows=16;s->fr_drop=1;s->dcim_copy=1;')
sub(C, ' const struct live_diag *D=&s->diag;uint8_t r=0;\n if(D->busy>d0->busy)r=13;',
    ' const struct live_diag *D=&s->diag;uint8_t r=0;\n if(f->flags&1)r=14;else if(D->busy>d0->busy)r=13;')
sub(C, '  c->ops.lease_end(c->ops.context,&source);\n  if(!valid){c->dropped++;s->copy_dropped++;goto finish;}',
    '  if(valid && s->fr_drop && (fr.torn_y|fr.torn_uv)){valid=0;s->fr_dropped++;fr.flags|=1;}\n'
    '  c->ops.lease_end(c->ops.context,&source);\n  if(!valid){c->dropped++;s->copy_dropped++;goto finish;}')

R = 'runtime.c'
sub(R, 'static void live_try_encode(struct live_runtime *r) {',
    '''/* DCIM/<f>/DSC_nnnn.JPG + LIVE/<f>/DSC_nnnn.MOV -> DCIM/<f>/LIV_nnnn.MOV (stock MTP lists DCIM only). */
static int live_dcim_copy(const live_file_ops *f,const char *still,const char *mov,uint8_t *buf,size_t cap){
 char dst[128];size_t n=0;while(still[n]&&n<120)n++;if(n<12||!buf||!cap)return 2;
 for(size_t i=0;i<=n;i++)dst[i]=still[i];
 size_t b=n-12;dst[b]='L';dst[b+1]='I';dst[b+2]='V';dst[b+3]='_';dst[n-3]='M';dst[n-2]='O';dst[n-1]='V';
 int in=-1,out=-1,rc=0;if(f->open_read(f->opaque,mov,&in))return 3;
 if(f->create_exclusive(f->opaque,dst,&out)){f->close(f->opaque,in);return 4;}
 for(;;){size_t got=0,w=0;if(f->read(f->opaque,in,buf,cap,&got)){rc=5;break;}if(!got)break;
  if(f->write(f->opaque,out,buf,got,&w)||w!=got){rc=6;break;}if(got<cap)break;if(f->yield)f->yield(f->opaque);}
 f->close(f->opaque,in);if(f->close(f->opaque,out)&&!rc)rc=7;if(rc)f->remove_owned(f->opaque,dst);return rc;}
static void live_try_encode(struct live_runtime *r) {''')
sub(R, ' r->transaction_error=(uint32_t)(-rc);if(!rc)r->transaction_count++;',
    ' r->transaction_error=(uint32_t)(-rc);if(!rc)r->transaction_count++;\n'
    ' if(!rc && r->stream.dcim_copy && r->stream.frozen_bytes){\n'
    '  r->stream.dcim_result=(uint32_t)live_dcim_copy(&ops,t.path,checkmov,r->stream.frozen_bytes,LIVE_BITSTREAM_BYTES);\n'
    '  if(!r->stream.dcim_result)r->stream.dcim_copies++;live_log(&r->stream,now_us(),EV_DCIM,r->stream.dcim_result,(uint64_t)fc.last_native_error);}')
print('v10 patch applied to', src)
