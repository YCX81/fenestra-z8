"""The fresh control batch must isolate preview while preserving native inputs."""
import ast,importlib.util,tempfile,struct,copy
from pathlib import Path
from types import SimpleNamespace
D=Path(__file__).resolve().parents[1]
source=ast.parse((D/'driver/round2/run_live.py').read_text())
contact=next(n for n in source.body if isinstance(n,ast.FunctionDef) and n.name=='contacts')
class Probe:
    @staticmethod
    def key_body():return bytes(124)
    @staticmethod
    def expected_keys(*args):return struct.pack('<11I',0x200,*([0]*10))
    @staticmethod
    def shutter_body(*args):
        b=bytearray(124);b[0]=bool(args[0]);b[1]=bool(args[1]);return bytes(b)
    @staticmethod
    def navigation_contact(key):
        body=bytearray(124)
        body[{'menu':0x43,'left':0x23,'right':0x22,'down':0x25,'up':0x24,'playback':0x44,'rec':9,'delete':0x45}[key]]=1
        sw1=0x2000 if key=='rec' else 0
        return bytes(body),struct.pack('<11I',0x200,sw1,*([0]*9))
def rows(mode):
    ns={'A':SimpleNamespace(mode=mode),'probe':Probe}
    exec(compile(ast.Module(body=[contact],type_ignores=[]),'driver-round2-contacts','exec'),ns)
    return ns['contacts'](None)[0]
play=rows('play');ctl=rows('noplay');base=rows('base')
assert ctl==base
assert len(play)==len(ctl)
assert all(a['label']==b['label'] and a['gap']==b['gap'] for a,b in zip(play,ctl))
diff=[a['label'] for a,b in zip(play,ctl) if a!=b]
assert diff==['pb-play-trigger'],diff
labels=[r['label'] for r in play]
assert labels.index('pb-preplay')<labels.index('pb-play-trigger')<labels.index('pb-play-restored')<labels.index('pb-play-return-shooting')<labels.index('pb-return-full')<labels.index('pb-return-shot-final')
assert not any(r['body'][0x45] for r in play)
assert sum(r['body'][0x44] for r in play)==2
assert sum(r['body'][9] for r in play)==1
assert not any(r['body'][9] for r in ctl)
trigger=next(r for r in play if r['label']=='pb-play-trigger')
control_trigger=next(r for r in ctl if r['label']=='pb-play-trigger')
assert trigger['body'][9]==1 and trigger['expected']==control_trigger['expected']
raw,unfiltered=Probe.navigation_contact('rec')
assert trigger['body']==raw  # Full external body is retained byte-for-byte.
assert [i for i,(a,b) in enumerate(zip(trigger['expected'],unfiltered)) if a!=b]==[5]
assert int.from_bytes(trigger['expected'][4:8],'little')^int.from_bytes(unfiltered[4:8],'little')==0x2000
assert all(a['expected']==b['expected'] for a,b in zip(play,ctl))
spec=importlib.util.spec_from_file_location('compare',D/'driver/round2/compare.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
from PIL import Image
with tempfile.TemporaryDirectory() as t:
    p=Path(t);a=Image.new('RGB',(1024,682));a.save(p/'a.png');a.save(p/'b.png')
    assert m.pixel_compare(p/'a.png',p/'b.png')['all_pixels_equal']
    a.putpixel((500,650),(255,0,0));a.save(p/'b.png');r=m.pixel_compare(p/'a.png',p/'b.png')
    assert not r['all_pixels_equal']
    assert r['regions']['bottom_chrome']['changed_pixels']==1
    assert r['regions']['central_photo']['changed_pixels']==0
# Balanced transient VG counters do not excuse a display object still linked
# to the old playback view once the native application has changed to view 3.
clean={'label':'final','native_view_id':3,'current_view':'0xa0282dab8','runtime':{
    'status':'validated heap runtime read-only','display.attached':0,'render_request':0,
    'images_created':48,'images_destroyed':48,'paths_created':48,'paths_destroyed':48,
    'paints_created':48,'paints_destroyed':48,'ui_detached':4,'unlinks':4,
    'after_leave_requests':0}}
assert m.lifecycle_state(clean)['native_object_leak'] is False
leaked=copy.deepcopy(clean);leaked['runtime']['display.attached']=1;leaked['runtime']['render_request']=2
r=m.lifecycle_state(leaked)
assert r['native_object_leak'] is True and r['attached_after_leave'] is True
assert r['resource_leak_after_leave'] is False and r['pending_render_request_after_leave'] is True
resource=copy.deepcopy(clean);resource['runtime']['images_destroyed']=47
assert m.lifecycle_state(resource)['resource_leak_after_leave'] is True
assert m.lifecycle_state(resource)['native_object_leak'] is True
still_playing=copy.deepcopy(leaked);still_playing['native_view_id']=2
assert m.lifecycle_state(still_playing)['native_object_leak'] is False
assert m.lifecycle_state({'label':'base-final'})['native_object_leak'] is None
# The authorized retry suffix keeps all three first-round directories distinct,
# while the orchestrator retains exactly the same fixture.raw input path.
batch_ast=ast.parse((D/'driver/round2/batch.py').read_text())
scenarios=next(n.value for n in batch_ast.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='scenarios' for t in n.targets))
names=[n.elts[0].value for n in scenarios.elts]
assert names==['pb-ctl-base','pb-ctl-noplay','pb2-play']
assert len(set(names+[n+'-retry' for n in names]))==6
assert "str(ROOT/'fixture.raw')" in (D/'driver/round2/batch.py').read_text()
print('ROUND2_DRIVER_PASS: identical native sequences; only raw REC differs, downstream native expectation clears only SW1 bit0x2000; no Delete; post-return shot; unmasked pixel/chrome comparison; native attachment/resource leaks surfaced; all six run names unique')
