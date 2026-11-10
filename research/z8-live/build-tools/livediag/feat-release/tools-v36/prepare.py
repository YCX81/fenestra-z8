"""Create fresh release copies, never edit reference trees."""
from pathlib import Path
import shutil, subprocess, sys, json, hashlib
root=Path(__file__).resolve().parent.parent
assert Path.cwd()==root
for a,b in [('src-fix32','src-fix36'),('pbfix/live-playback-ui-icon','pb-fix36')]:
    shutil.copytree(a,b)
icons=root/'icons-v36'
for name in ['icon32_generate.py','icon32_patch.py','live-badge.svg','icon32_layout.h','icon32_alpha.h','icon32_indicator.c.inc','icon32_pb.c.inc','icon32_pb_gate.c.inc','icon32_pb_writer.c.inc','README-icon32.md']:
    shutil.copyfile(root.parent/'feat-icon'/name, icons/name)
shutil.copyfile(root.parent/'feat-icon/icon32-assets/geometry.json',icons/'geometry-reference.json')
# The original patch limits output to its own folder. Keep the same checks,
# expanding only to this release workspace; hashes/patch contents are unchanged.
p=icons/'icon32_patch.py';t=p.read_text();t=t.replace('p.is_relative_to(ROOT)', 'p.is_relative_to(ROOT.parent)').replace('target.is_relative_to(ROOT)', 'target.is_relative_to(ROOT.parent)');p.write_text(t)
g=json.loads((icons/'geometry-reference.json').read_text())
args=[]
for side in ('shoot','pb'):
    for key in ('width','height','dest_width','dest_height','y'):
        args += ['--'+side+'-'+key.replace('_','-'),str(g[side][key])]
    key='center_x' if side=='shoot' and g[side].get('center_x') is not None else 'x'
    args += ['--'+side+'-'+key.replace('_','-'),str(g[side][key])]
    for target in g['targets'][side]: args += ['--'+side+'-target',','.join(map(str,target))]
subprocess.run([sys.executable,'-B',str(icons/'icon32_generate.py'),*args],check=True)
actual=json.loads((icons/'icon32-assets/geometry.json').read_text())
assert all(actual[k]==g[k] for k in ('shoot','pb','targets','design','raster'))
subprocess.run([sys.executable,'-B',str(icons/'icon32_patch.py'),'src-fix36','pb-fix36'],check=True)
p=root/'src-fix36/runtime.h';t=p.read_text();assert t.count('log_err,log_pad')==1
p.write_text(t.replace(' uint32_t log_saved_head,log_saves,log_err,log_pad;', ''' /* v36: former log_pad is a cached RMW word; boot zeroes it once.
  * Do not reset on encoder close/reopen. Runtime size/other offsets unchanged. */
 uint32_t log_saved_head,log_saves,log_err,w5_close_lock;'''))
p=root/'src-fix36/codec/live_w5enc.c';t=p.read_text()
t=t.replace('#include "live_codec.h"','#include "live_codec.h"\n#include "../runtime.h"\n#include "../ui/live_ui.h"')
t=t.replace('uint32_t close_lock, close_failed;', 'uint32_t reserved_close_lock, close_failed;')
t=t.replace('    uint32_t flags, alias_slot_ok, alias_slot_fail, alias_bs_ok, alias_bs_fail, staging_scratch;\n};','''    uint32_t flags, alias_slot_ok, alias_slot_fail, alias_bs_ok, alias_bs_fail, staging_scratch;
    struct live_runtime *rmw; /* v36: cached owner, occupies former tail padding */
};''')
t=t.replace('&w->close_lock','&w->rmw->w5_close_lock')
t=t.replace('    void *ad, *bd; uint64_t a, b; uint32_t an, bn;', '''    /* Resolve the single cached owner before allocating/publishing pool state.
     * Pool memory cannot support exclusive RMW on this hardware. */
    struct live_runtime *r = live_ui_engine_get();
    if (!r || (uintptr_t)r < UINT64_C(0xa00000000) ||
        (uintptr_t)r > UINT64_C(0xa40000000) - sizeof(*r) || ((uintptr_t)r & 7u) ||
        r->magic != LIVE_RUNTIME_MAGIC || r->size != sizeof(*r) ||
        e != &r->stream.encoder) return -1;
    void *ad, *bd; uint64_t a, b; uint32_t an, bn;''')
t=t.replace('    memset(w, 0, sizeof(*w));','    memset(w, 0, sizeof(*w));\n    w->rmw = r; /* bound before e->hw publication; never reset the shared lock */')
t=t.replace('/* stock entry points', '''_Static_assert(sizeof(struct live_w5) == 1053600, "v32 pool header size");
_Static_assert(offsetof(struct live_w5, rmw) == 1053592, "v32 tail padding");
_Static_assert(sizeof(struct live_runtime) == 23184, "v32 runtime ABI");
_Static_assert(offsetof(struct live_runtime, w5_close_lock) == 18908, "v32 log_pad slot");

/* stock entry points''')
p.write_text(t)
print('v36 source copies prepared')
