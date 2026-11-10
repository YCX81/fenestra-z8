"""v9b: normalize only reviewed RMW relocation; allocation is checked verbatim."""
def without_allocation(text):
    text=text.replace('#include "../runtime.h"\n#include "../ui/live_ui.h"\n','')
    text=text.replace('reserved_close_lock','close_lock').replace('reserved_diag_n','diag_n')
    text=text.replace('&w->rmw->w5_close_lock','&w->close_lock').replace('&w->rmw->w5_diag_n','&w->diag_n')
    text=text.replace('    struct live_runtime *rmw; /* v9b: uses former tail padding; pool layout unchanged */\n','')
    a=text.find('_Static_assert(sizeof(struct live_w5)')
    if a>=0:
        b=text.index('/* stock entry points',a);text=text[:a]+text[b:]
    a=text.find('    /* Single runtime owns this encoder')
    if a>=0:
        b=text.index('    void *ad, *bd;',a);text=text[:a]+text[b:]
    return text.replace('    w->rmw = r; /* set before e->hw publication and any RMW */\n','')
