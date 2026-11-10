"""v9 permits allocation/lifetime changes only in pool() and live_encoder_init()."""
import re
def without_allocation(text):
    # Keep the rest byte-for-byte, including all shared close/frame/preempt code.
    a=text.index('/* Live photo-mode encoder');b=text.index('#include "live_codec.h"',a)
    text=text[:a]+text[b:]
    for start,end in [('static int pool(', '/* 64-byte bursts'),('int live_encoder_init(', '/* Bring-up hook')]:
        a=text.index(start);b=text.index(end,a);text=text[:a]+text[b:]
    return re.sub(r'^#define (HEAP_ALLOC|HEAP_FREE|POOL_FREE)\([^\n]*\n','',text,flags=re.M)
