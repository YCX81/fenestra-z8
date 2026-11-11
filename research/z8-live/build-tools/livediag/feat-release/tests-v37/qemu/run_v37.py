"""One QEMU run of the v37 Live image: real MCU key frames only, read-only guest
observation, no guest RAM writes. Emulation only; proves nothing about hardware.

Scenario (--mode combo):
  JPEG fine -> Setup > Fenestra > Live Photo > On -> wait Ready
  icon: MENU open/close, screenshots after return to the shooting view
  shoot one Live photo, wait for the save
  playback: badge, REC plays the preview stored in the MOV, a key press hides the
            badge and it returns, REC plays again, leave playback
  icon: screenshots after return from playback
Optional steps: --pose left|right (virtual orientation sensor), --movie (photo ->
movie -> photo selector; the stock firmware itself errors there in about a third of
QEMU runs, so it is a separate opt-in).

Derived from codex-tasks/live-playback-ui/driver/round2/run_live.py (2026-11-04)."""
import argparse,hashlib,json,os,shutil,struct,subprocess,sys,time,threading
from pathlib import Path
from types import SimpleNamespace
SOURCE=Path(__file__).resolve().parent
NIKON=Path(os.environ.get('Z8_NIKON','/mnt/c/Users/ycx/Desktop/Nikon'))
HELPERS=NIKON/'_bundle-20261102/codex-tasks/live-playback-ui/driver'
SNAP=NIKON/'.round2-20261018/A-runtime/integration/serial/deps/U-live01'
ROOT=SNAP/'project'
BASE=Path.home()/'.local/share/nikon-z8-qemu'
RUNROOT=BASE/'v37-20261111'
STOCK=NIKON/'z8-integration-20261014/.local/firmware/z8-c311-main.bin'
P=argparse.ArgumentParser()
P.add_argument('--name',required=True)
P.add_argument('--main',type=Path,required=True)
P.add_argument('--symbols',type=Path,required=True,help='changed-ranges.json of the pool stage (payload symbols)')
P.add_argument('--offsets',type=Path,required=True,help='offsets.py output')
P.add_argument('--mode',choices=['combo','off','ready'],default='combo')
P.add_argument('--pose',choices=['left','right'])
P.add_argument('--movie',action='store_true')
P.add_argument('--seconds',type=int,default=900)
P.add_argument('--qemu',type=Path,default=BASE/'qemu-merged/build/qemu-system-aarch64')
P.add_argument('--keep-large',action='store_true')
A=P.parse_args()
if not A.name or Path(A.name).name!=A.name:raise ValueError('single run-directory name required')
if not 30<=A.seconds<=1800:raise ValueError('host deadline 30..1800 seconds required')
if shutil.disk_usage('/mnt/c').free<30*1024**3:raise RuntimeError('C: below 30 GiB')
meminfo=dict(line.split(':',1) for line in Path('/proc/meminfo').read_text().splitlines())
if int(meminfo['MemAvailable'].split()[0])*1024<2.5*1024**3:raise RuntimeError('Less than 2.5 GiB available')
RUNROOT.mkdir(exist_ok=True)
OUT=RUNROOT/A.name
if OUT.exists():raise FileExistsError(OUT)
os.environ['TMPDIR']=str(RUNROOT)
os.environ['FX_ICOUNT_SLEEP']='off'
sys.dont_write_bytecode=True
sys.path.insert(0,str(ROOT/'tools'));sys.path.insert(0,str(HELPERS))
import z8_capture_probe as probe
import pipe_transport
_init=pipe_transport.PipeIO.__init__
def bounded_init(self,base,timeout=2):_init(self,base,timeout=timeout)
pipe_transport.PipeIO.__init__=bounded_init
pipe_transport.install()
from PIL import Image
import virtual_recording_card as vrc,exfat_put
_create=vrc.create
def card_create(path,fs='blank'):
    result=_create(path,fs)
    result['fenestra_card']=exfat_put.put(path,'FENESTRA',{})
    return result
vrc.create=card_create
SYMBOLS=json.loads(A.symbols.read_text())['symbols']
OFFSETS=json.loads(A.offsets.read_text())
LIVE_CONTACTS=[]
TAB_ENTERED=False
QUALITY_SELECTED=False
LIVE_SELECTED=False
NATIVE_IDS={}
def native_text_ids(g):
    """Resolve exact English IDs through Nikon's installed text table, read-only."""
    if NATIVE_IDS:return NATIVE_IDS
    rd=lambda a,n:int.from_bytes(g.read_bytes(a,n),'little')
    wanted={'quality_settings':(0x432b92f0,'Image quality settings'),
            'quality':(0x432a0258,'Image quality'),'jpeg_fine':(0x432a5f30,'JPEG fine'),
            'photo_title':(0x432a7fac,'PHOTO SHOOTING MENU')}
    count=rd(0xa0281cd5c,4)
    if count==65535:count=3136
    if not 1000<=count<=8192:return {}
    poolroot=rd(0xa02f2ac38,8);offsetroot=rd(0xa02f2ac48,8)
    if not poolroot or not offsetroot:return {}
    pools=struct.unpack('<36Q',g.read_bytes(poolroot,288))
    arrays=struct.unpack('<36Q',g.read_bytes(offsetroot,288))
    candidates=sorted(((pool,i) for i,pool in enumerate(pools) if 0x40160000<=pool<=0x432a0258),reverse=True)
    for pool,language in candidates:
        if 0x432b92f0-pool>1024*1024:continue
        offsets=struct.unpack('<'+str(count+1)+'I',g.read_bytes(arrays[language],4*(count+1)))
        ids={name:next((i for i,o in enumerate(offsets[:-1]) if pool+o==address),None) for name,(address,text) in wanted.items()}
        if any(i is None for i in ids.values()):continue
        NATIVE_IDS.update(ids);return NATIVE_IDS
    return {}
def direction(menu,target):
    selected,count=menu.get('selected',0),menu.get('count',0)
    if count<=0 or not 0<=target<count:return None
    if selected==target:return 'ok'
    return 'down' if (target-selected)%count<=(selected-target)%count else 'up'
def menu_state(g):
    rd=lambda a,n:int.from_bytes(g.read_bytes(a,n),'little')
    valid=lambda a,n:0xa00000000<=a<=0xa40000000-n
    stack=rd(0xa032b5160,8)
    if not valid(stack,24):return {'page':0}
    stride,count,data=rd(stack,4),rd(stack+8,4),rd(stack+16,8)
    if not 8<=stride<=256 or not 1<=count<=64 or not valid(data,count*stride):return {'page':0}
    page=rd(data+(count-1)*stride,8)
    if not valid(page,96):return {'page':0}
    identity=rd(page,4);descriptor=rd(page+80,8)
    result=dict(page=hex(page),page_id=identity>>16,selected=rd(page+50,2)+rd(page+88,2))
    if valid(descriptor,64):
        rows=rd(descriptor+16,8);size=rd(descriptor+32,2);index=result['selected']
        result['count']=size
        if identity >> 16 == 25 and result['selected'] > 0: result['selected'] -= 1
        if 0<size<=256 and (valid(rows,size*44) or 0x40160000<=rows<=0x45000000-size*44):
            data=g.read_bytes(rows,size*44)
            result['rows']=[dict(index=i,text_id=int.from_bytes(data[i*44:i*44+4],'little'),row_id=int.from_bytes(data[i*44+24:i*44+28],'little')) for i in range(size)]
            if index<size:result.update({k:v for k,v in result['rows'][index].items() if k!='index'})
        header=rd(descriptor+8,8)
        if 0x40160000<=header<0x45000000 or valid(header,72):result['title_id']=rd(header+68,4)
    return result
def adaptive_contact(g,label,info):
    global TAB_ENTERED,QUALITY_SELECTED,LIVE_SELECTED
    item=next((x for x in LIVE_CONTACTS if label.endswith('-'+x['label'])),None)
    if not item or item['label'].endswith('-release'):return
    kind=item['label'];key=None;menu=info['menu'];page=menu.get('page_id',0)
    if kind.startswith('quality-seek-'):
        if not QUALITY_SELECTED:
            ids=dict(native_text_ids(g));ids.update(photo_title=1105,quality_settings=2417,quality=760,jpeg_fine=762)
            title=menu.get('title_id');target=None
            if page==45:
                key=direction(menu,8)
                if key=='ok':QUALITY_SELECTED=True
            elif title==ids.get('photo_title'):target=1800
            elif title==ids.get('quality_settings'):target=ids.get('quality')
            elif title==ids.get('quality'):target=ids.get('jpeg_fine')
            matches=[r for r in menu.get('rows',[]) if target is not None and r['text_id']==target]
            if matches:
                chosen=min(matches,key=lambda r:min((r['index']-menu['selected'])%menu['count'],(menu['selected']-r['index'])%menu['count']))
                key=direction(menu,chosen['index'])
                if key=='ok' and target==ids.get('jpeg_fine'):QUALITY_SELECTED=True
    elif kind.startswith('tab-seek-'):
        if not TAB_ENTERED:
            if page==1:
                key=direction(menu,4)
                if key=='ok':key='right';TAB_ENTERED=True
            elif page==663:key='right';TAB_ENTERED=True
            elif page:key='down'
    elif kind.startswith('live-seek-'):
        if not LIVE_SELECTED:
            target={663:0x7ff0,0x7000:0x7fb2,0x705e:(0x7fb1 if A.mode=='off' else 0x7fb0)}.get(page)
            chosen=next((r['index'] for r in menu.get('rows',[]) if r['row_id']==target),None)
            if chosen is not None:
                key=direction(menu,chosen)
                if page==0x705e and key=='ok':LIVE_SELECTED=True
    else:return
    if key:body,expected=probe.navigation_contact(key)
    else:body,expected=probe.key_body(),probe.expected_keys()
    item['body'],item['expected']=body,expected
    if not key:
        item['gap']=10000000
        release=next((x for x in LIVE_CONTACTS if x['label']==kind+'-release'),None)
        if release:release['gap']=10000000
    info['adaptive_key']=key or 'release'
def sha(path):
    h=hashlib.sha256()
    with Path(path).open('rb') as f:
        while b:=f.read(1024*1024):h.update(b)
    return h.hexdigest()
def fields(g,address,layout,skip=()):
    rd=lambda a,n:int.from_bytes(g.read_bytes(a,n),'little')
    return {k:rd(address+v,4) for k,v in layout.items() if k!='bytes' and k not in skip}
def snapshot(g,label):
    rd=lambda address,size:int.from_bytes(g.read_bytes(address,size),'little')
    ram=lambda a,n:a%8==0 and 0xa00000000<=a<=0xa40000000-n
    s={'label':label,'photo_movie':rd(0xa03bd062c,4),'menu':menu_state(g),'pose':rd(0xa03bc8004,4),'pool_selector':rd(0xa03f31a68,4),'audio_service':rd(0xa031f8d28,4),'audio_driver':rd(0xa02d4cb48,4)}
    view=rd(0xa032280c8,8);s['view']=hex(view)
    if ram(view,0x110):s['view_id']=rd(view+0x54,4);s['view_state']=rd(view+0xd0,4)
    root=rd(0xa032b50c8+16,8)
    if not ram(root,664*96):return s
    core=rd(root+663*96+80,8)
    if core==0xa028ca040 or not ram(core,2536) or rd(core+64,4)!=0x465a394d:return s
    registry=rd(core+2528,8)
    if not ram(registry,24) or rd(registry,4)!=0x47525846:return s
    u=rd(registry+8,8);pb=rd(registry+16,8)
    if ram(u,OFFSETS['ui']['bytes']) and rd(u,4)==0x4c50554b:
        s['ui']=fields(g,u,OFFSETS['ui'],skip=('engine',))
        engine=rd(u+OFFSETS['ui']['engine'],8)
        if ram(engine,OFFSETS['live']['bytes']) and rd(engine+4,4)==OFFSETS['live']['bytes']:
            s['live']=fields(g,engine,OFFSETS['live'])
    if ram(pb,OFFSETS['pb']['bytes']) and rd(pb,4)==0x50425549 and rd(pb+4,4)==OFFSETS['pb']['bytes']:
        s['pb']=fields(g,pb,OFFSETS['pb'],skip=('native_path',))
        s['pb']['native_path']=g.read_bytes(pb+OFFSETS['pb']['native_path'],64).split(b'\0',1)[0].decode('ascii','replace')
    return s
class Guest(probe.PreparedGuest):
    def __init__(self,*args,**kw):
        super().__init__(*args,**kw)
        self.command[self.command.index('-kernel')+1]=str(A.main.resolve())
        at=self.command.index('-icount')+1
        self.command[at]=self.command[at].replace('sleep=on','sleep=off')
        self.data['v37']=dict(mode=A.mode,pose=A.pose,movie=A.movie,executed_main_sha256=sha(A.main),
            executed_main=str(A.main.resolve()),qemu_sha256=sha(A.qemu),flashable=False)
        self.data['command']=self.command
        print('EXECUTED_MAIN',json.dumps(self.data['v37']),flush=True)
        shutil.copy2(Path(__file__),self.out/Path(__file__).name)
    def __enter__(self):
        g=super().__enter__();original=g.qmp.command
        def watchdog():
            deadline=time.monotonic()+A.seconds+30
            while g.proc.poll() is None:
                if (g.out/'stop.request').exists() or time.monotonic()>deadline:g.proc.terminate();return
                time.sleep(1)
        threading.Thread(target=watchdog,daemon=True).start()
        def command(name,**kw):
            result=original(name,**kw)
            if name=='screendump':
                path=Path(kw['filename']);Image.open(path).save(path.with_suffix('.png'));path.unlink()
                info=snapshot(g,path.stem)
                adaptive_contact(g,path.stem,info)
                if A.pose and path.stem.endswith('-pose-set'):
                    sample={'left':'0000c0000000','right':'000040000000'}[A.pose]
                    try:original('qom-set',path='/machine',property='orientation-input',value=sample);info['pose_input']=sample
                    except RuntimeError as e:info['pose_input_error']=str(e)
                if A.pose and path.stem.endswith('-pose-clear'):
                    try:original('qom-set',path='/machine',property='orientation-input',value='c00000000000');info['pose_input']='landscape'
                    except RuntimeError as e:info['pose_input_error']=str(e)
                with (g.out/'live-checkpoints.jsonl').open('a') as f:f.write(json.dumps(info)+'\n')
                brief={k:info.get(k) for k in ('label','pool_selector','audio_service','audio_driver','photo_movie','view_id','view_state','pose','ui','pb','adaptive_key')}
                brief['live']={k:v for k,v in info.get('live',{}).items()}
                print('CHECKPOINT',json.dumps(brief),flush=True)
                if path.stem.endswith('-photo-live-10s'):
                    if not QUALITY_SELECTED:raise RuntimeError('JPEG fine menu selection was not reached')
                    if not LIVE_SELECTED or info.get('live',{}).get('requested')!=(0 if A.mode=='off' else 1):
                        raise RuntimeError('Menu navigation did not set Live')
            return result
        g.qmp.command=command
        return g
probe.PreparedGuest=Guest
class Peer(probe.CapturePeer):
    def __init__(self,*args,**kw):
        super().__init__(*args,**kw);self.trace_points={}
        self.record['limit_frames']=16384   # long scenario; the stock bound is 1024 frames
probe.CapturePeer=Peer
probe.MENU_CONTACTS['ok']=(0x2a,0x8000,0)
probe.MENU_CONTACTS['playback']=(0x44,0,0x2)
probe.MENU_CONTACTS['rec']=(0x09,0,0x2000)
def contacts(args):
    global LIVE_CONTACTS
    rows=[]
    def shutter(label,half=False,full=False,gap=1):
        rows.append(dict(label=label,body=probe.shutter_body(half,full),expected=probe.expected_keys(half,full),
                         gap=int(gap*1e9),half=half,full=full))
    def key(label,key,gap=.5,consumed=False):
        body,expected=probe.navigation_contact(key)
        if consumed:   # REC on a Live photo is taken by the playback layer before the native decoder
            decoded=bytearray(expected);sw1=int.from_bytes(decoded[4:8],'little')
            decoded[4:8]=(sw1&~0x2000).to_bytes(4,'little');expected=bytes(decoded)
        rows.append(dict(label=label,body=body,expected=expected,gap=400000000,half=False,full=False))
        rows.append(dict(label=label+'-release',body=probe.key_body(),expected=probe.expected_keys(),
                         gap=int(gap*1e9),half=False,full=False))
    def movie_selector(label,on,gap):
        body=bytearray(probe.key_body());body[0x57]=int(on)
        keys=bytearray(probe.expected_keys());keys[38]=int(on)
        rows.append(dict(label=label,body=bytes(body),expected=bytes(keys),gap=int(gap*1e9),half=False,full=False))
    shutter('wake-half',True);shutter('wake-release')
    key('quality-menu','menu',10);key('quality-left','left');key('quality-right','right')
    for i in range(18):key(f'quality-seek-{i:02}','down')
    shutter('quality-exit-half',True);shutter('quality-exit-release')
    key('ui-menu','menu',10);key('ui-left','left')
    for i in range(7):key(f'tab-seek-{i:02}','down')
    for i in range(20):key(f'live-seek-{i:02}','up')
    key('ui-exit-menu','menu',10)
    shutter('ui-exit-half',True);shutter('ui-exit-release')   # make sure the menu is closed
    shutter('photo-live-10s',gap=10)
    start=len(rows)
    if A.mode=='ready':
        shutter('ready-a',gap=5);shutter('ready-b',gap=5);shutter('final',gap=1)
        LIVE_CONTACTS=rows
        return rows,start
    if A.mode=='combo':
        shutter('icon-ready',gap=2)
        key('icon-menu-open','menu',2.5)
        key('icon-menu-close','menu',.4)
        shutter('icon-after-menu-a',gap=1);shutter('icon-after-menu-b',gap=2.5);shutter('icon-after-menu-c',gap=1)
        if A.pose:
            shutter('pose-set',gap=3);shutter('icon-pose-a',gap=2);shutter('pose-clear',gap=3);shutter('icon-pose-b',gap=2)
        if A.movie:
            movie_selector('movie-on',True,8);movie_selector('movie-view',True,2)
            movie_selector('movie-off',False,6);shutter('icon-after-movie-a',gap=3);shutter('icon-after-movie-b',gap=4)
    shutter('half-press',True,gap=2)
    shutter('full-press',True,True,gap=.15)
    shutter('full-release',gap=.15)
    shutter('post-photo-a',gap=10);shutter('post-photo-b',gap=15);shutter('post-photo-c',gap=15)
    if A.mode=='combo':
        key('pb-enter','playback',4)
        shutter('pb-badge',gap=1)
        key('pb-play-trigger','rec',.6,consumed=True)
        shutter('pb-frame-a',gap=.8);shutter('pb-frame-b',gap=.8);shutter('pb-frame-c',gap=2.5)
        shutter('pb-restored',gap=1.5)
        key('pb-info-down','down',.3)
        shutter('pb-key-hidden',gap=1.5);shutter('pb-key-back',gap=1)
        key('pb-info-up','up',2)
        key('pb-replay','rec',1.2,consumed=True)
        shutter('pb-replay-frame',gap=3.5);shutter('pb-replay-done',gap=1)
        key('pb-exit','playback',.5)
        shutter('icon-after-pb-a',gap=1.5);shutter('icon-after-pb-b',gap=3);shutter('icon-after-pb-c',gap=2)
    shutter('final',gap=1)
    LIVE_CONTACTS=rows
    return rows,start
probe.contact_sequence=contacts
recipe=json.loads((SNAP/'bundle.json').read_text())['recipe']
names={s.split('=')[0] for s in probe.load_model_profile()['properties']}
names|={'pcie-link-diagnostic-mask','pcie-dbi-model','pcie-ram-dma-input','pcie-image-ram-dma-input','pcie-intx-model','sensor-frame-tag-model'}
extra=[s for s in recipe['properties'] if s.split('=')[0] not in names and ':' not in s]
extra+=['z8-serial5-config-model=on','engine153-alternate-bank-model=on','z8-jpeg-decode=on',
        'wave5-upstream-model=on','wave5-host-codec-diagnostic=on']   # v32+ Live encodes with Wave5 (host x264)
profile=RUNROOT/(A.name+'-properties.json')
profile.write_text(json.dumps(dict(model='Nikon Z8',main_sha256=sha(STOCK),properties=extra),indent=2)+'\n')
args=SimpleNamespace(firmware=STOCK,qemu=A.qemu,raw=BASE/'runs/raw-precapture-20261012/input14.raw',output=OUT,seconds=A.seconds,
    settle=1,half_seconds=2,capture_properties=profile,card=True,nvme_trace=False,gpio=True,rtc_status=True,
    tagged_input=True,capture_selector=True,capture_timing_input=True,hsfc_processing_input=False,menu_actions='',menu_only=True,
    wait_liveview=False,navigation_press_ms=400,navigation_release_ms=500,capture_pixels=False,wait_capture_work=False,
    skip_cseq_repeat=True,native_writer_observer=False)
try:probe.run(args)
finally:
    if OUT.exists():
        for p in OUT.glob('*.ppm'):Image.open(p).save(p.with_suffix('.png'));p.unlink()
        card=OUT/'capture-card.raw'
        if card.exists():
            from card_export import catalog,extract
            try:
                inventory=catalog(card);(OUT/'card-catalog.json').write_text(json.dumps(inventory,indent=2)+'\n')
                exports=OUT/'files';exports.mkdir()
                for row in inventory['files']:
                    if row['directory'] or not row['bytes']:continue
                    dest=exports/row['path'].lstrip('/');dest.parent.mkdir(parents=True,exist_ok=True)
                    extract(card,row['path'],dest)
            except Exception as e:
                (OUT/'card-export-error.txt').write_text(repr(e)+'\n');print('CARD_EXPORT_ERROR',repr(e),flush=True)
        removed=[]
        if not A.keep_large:
            for p in OUT.rglob('*'):
                if p.is_file() and p.suffix.lower() in ('.raw','.bin') and p.stat().st_size>1024*1024:
                    removed.append(dict(path=str(p),bytes=p.stat().st_size));p.unlink()
        (OUT/'cleanup.json').write_text(json.dumps(removed,indent=2)+'\n')
        print('Finished',OUT,flush=True)
