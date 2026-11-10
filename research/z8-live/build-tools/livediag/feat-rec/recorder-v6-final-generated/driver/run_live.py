"""One final-batch run; actual menu inputs, stock preparation, pinned overlay kernel.
No guest RAM writes, no plugins, no Live readiness manipulation. Emulation only.
"""
import argparse,hashlib,json,os,shutil,struct,subprocess,sys,time,threading
from pathlib import Path
from types import SimpleNamespace
HERE=Path(__file__).resolve().parent
TASK=HERE.parent
NIKON=Path('/mnt/c/path/to/user/Desktop/Nikon')
SNAP=NIKON/'.round2-20261018/A-runtime/integration/serial/deps/U-live01'
ROOT=SNAP/'project'
BASE=Path.home()/'.local/share/nikon-z8-qemu'
RUNROOT=BASE/'live-a-driver-20261104'
STOCK=NIKON/'z8-integration-20261014/.local/firmware/z8-c311-main.bin'
PY=BASE/'integration-20261014/venv/bin/python'
P=argparse.ArgumentParser()
P.add_argument('--name',required=True)
P.add_argument('--main',type=Path,required=True)
P.add_argument('--mode',choices=['off','on','two'],required=True)
P.add_argument('--seconds',type=int,default=900)
P.add_argument('--qemu',type=Path,default=NIKON/'.live-photo-20261030/audio-qemu/qemu-system-aarch64')
P.add_argument('--retry-offsets',action='store_true')
P.add_argument('--preflight',action='store_true',help='Construct and validate complete guest command/resources, never launch QEMU')
P.add_argument('--keep-large',action='store_true',help='Debug-only opt-out from normal finished-run cleanup')
A=P.parse_args()
if not A.name or Path(A.name).name!=A.name:raise ValueError('single run-directory name required')
if not 30<=A.seconds<=900:raise ValueError('host deadline 30..900 seconds required')
print(subprocess.check_output(['df','-h','/mnt/c'],text=True),flush=True)
if shutil.disk_usage('/mnt/c').free<30*1024**3:raise RuntimeError('C: below 30 GiB')
meminfo=dict(line.split(':',1) for line in Path('/proc/meminfo').read_text().splitlines())
if int(meminfo['MemAvailable'].split()[0])*1024<2.5*1024**3:raise RuntimeError('Less than 2.5 GiB available')
active=0
for ent in Path('/proc').iterdir():
    if not ent.name.isdigit():continue
    try:
        cmd=(ent/'cmdline').read_bytes().split(b'\0')[0]
        if b'qemu' not in Path(os.fsdecode(cmd)).name.encode():continue
        active+=1
        elapsed=subprocess.check_output(['ps','-p',ent.name,'-o','etimes='],text=True).strip()
        if elapsed and int(elapsed)<90 and not A.preflight:raise RuntimeError(f'QEMU {ent.name} started <90 seconds ago')
    except (FileNotFoundError,ProcessLookupError,PermissionError):pass
if active>=10 and not A.preflight:raise RuntimeError('10 QEMU processes already running')
RUNROOT.mkdir(exist_ok=True)
OUT=RUNROOT/A.name
if OUT.exists():raise FileExistsError(OUT)
os.environ['TMPDIR']=str(RUNROOT)
os.environ['FX_ICOUNT_SLEEP']='off'
sys.dont_write_bytecode=True
sys.path.insert(0,str(ROOT/'tools'));sys.path.insert(0,str(HERE))
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
OFFSET_PATH=TASK/'runtime-offsets.json'
OFFSETS=json.loads(OFFSET_PATH.read_text())
LIVE_CONTACTS=[]
TAB_ENTERED=False
QUALITY_SELECTED=False
NATIVE_IDS={}
def native_text_ids(g):
    """Resolve exact English IDs through Nikon's installed text table, read-only."""
    if NATIVE_IDS:return NATIVE_IDS
    rd=lambda a,n:int.from_bytes(g.read_bytes(a,n),'little')
    wanted={'quality_settings':(0x432b92f0,'Image quality settings'),
            'quality':(0x432a0258,'Image quality'),'jpeg_fine':(0x432a5f30,'JPEG fine'),
            'photo_title':(0x432a7fac,'PHOTO SHOOTING MENU')}
    count=rd(0xa0281cd5c,4)
    if count==65535:count=3136  # Original sentinel; pinned stock IDs end3135.
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
        for name,(address,text) in wanted.items():
            if g.read_bytes(address,len(text)*2)!=text.encode('utf-16le'):raise RuntimeError('Pinned English label bytes changed')
        NATIVE_IDS.update(ids);return NATIVE_IDS
    return {}

def direction(menu,target):
    selected,count=menu.get('selected',0),menu.get('count',0)
    if count<=0 or not 0<=target<count:return None
    if selected==target:return 'ok'
    return 'down' if (target-selected)%count<=(selected-target)%count else 'up'

def menu_state(g):
    """Read-only translation of403dbdb4/403e9a90, plus native selected row."""
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
        # Native selected is one-based after the first movement (observed vs scanout).
        if identity >> 16 == 25 and result['selected'] > 0: result['selected'] -= 1
        if 0<size<=256 and (valid(rows,size*44) or 0x40160000<=rows<=0x45000000-size*44):
            data=g.read_bytes(rows,size*44)
            result['rows']=[dict(index=i,text_id=int.from_bytes(data[i*44:i*44+4],'little'),row_id=int.from_bytes(data[i*44+24:i*44+28],'little')) for i in range(size)]
            if index<size:result.update({k:v for k,v in result['rows'][index].items() if k!='index'})
        header=rd(descriptor+8,8)
        if 0x40160000<=header<0x45000000 or valid(header,72):result['title_id']=rd(header+68,4)
    return result

def adaptive_contact(g,label,info):
    global TAB_ENTERED,QUALITY_SELECTED
    item=next((x for x in LIVE_CONTACTS if label.endswith('-'+x['label'])),None)
    if not item or item['label'].endswith('-release'):return
    kind=item['label'];key=None;menu=info['menu'];page=menu.get('page_id',0);row=menu.get('row_id',0)
    if kind.startswith('quality-seek-'):
        if not QUALITY_SELECTED:
            ids=dict(native_text_ids(g));ids.update(photo_title=1105,quality_settings=2417,quality=760, jpeg_fine=762);info['native_text_ids']=ids
            title=menu.get('title_id');target=None
            if page==45:
                key=direction(menu,8)  # Screenshot: normal at 10, JPEG/HEIF fine at 8.
                if key=='ok': QUALITY_SELECTED=True
            elif title==ids.get('photo_title'):target=1800
            elif title==ids.get('quality_settings'):target=ids.get('quality')
            elif title==ids.get('quality'):target=ids.get('jpeg_fine')
            matches=[r for r in menu.get('rows',[]) if target is not None and r['text_id']==target]
            if matches:
                chosen=min(matches,key=lambda r:min((r['index']-menu['selected'])%menu['count'],(menu['selected']-r['index'])%menu['count']))
                key=direction(menu,chosen['index'])
                if key=='ok' and target==ids.get('jpeg_fine'):
                    QUALITY_SELECTED=True;info['jpeg_fine_selection']=chosen
    elif kind.startswith('tab-seek-'):
        if not TAB_ENTERED:
            if page==1:
                # Native tab selector retains page1 while selected tracks the tab.
                key=direction(menu,4)
                if key=='ok':key='right';TAB_ENTERED=True
            elif page==663:key='right';TAB_ENTERED=True
            elif page:key='down'
    elif kind.startswith('live-seek-'):
        if info.get('runtime',{}).get('requested')!=1:
            target={663:0x7ff0,0x7000:0x7fb2,0x705e:0x7fb0}.get(page)
            chosen=next((r['index'] for r in menu.get('rows',[]) if r['row_id']==target),None)
            if chosen is not None:key=direction(menu,chosen)
    else:return
    if key:body,expected=probe.navigation_contact(key)
    else:body,expected=probe.key_body(),probe.expected_keys()
    item['body'],item['expected']=body,expected
    if not key:
        item['gap']=10000000
        release=next((x for x in LIVE_CONTACTS if x['label']==kind+'-release'),None)
        if release:release['gap']=10000000
    info['adaptive_key']=key or 'release';info['adaptive_basis']=menu
def sha(path):
    h=hashlib.sha256()
    with Path(path).open('rb') as f:
        while b:=f.read(1024*1024):h.update(b)
    return h.hexdigest()
def snapshot(g,label):
    rd=lambda address,size:int.from_bytes(g.read_bytes(address,size),'little')
    s={'label':label,'photo_movie':rd(0xa03bd062c,4),'menu':menu_state(g),'jpeg_fine_selected_via_menu':QUALITY_SELECTED}
    root=rd(0xa032b50c8+16,8);s['root']=hex(root)
    if not root:return s
    state=rd(root+663*96+80,8);s['descriptor']=hex(state)
    if state==0xa028ca040 or state<0xa00000000 or state>=0xa40000000:return s
    s.update(core_magic=hex(rd(state+64,4)),core_installed=rd(state+68,4),core_count=rd(state+32,2))
    u=rd(state+2528,8);s['ui']=hex(u)
    if u<0xa00000000 or u>=0xa40000000:return s
    s.update(ui_magic=hex(rd(u,4)),save_error=rd(u+28,4),loaded=rd(u+32,4),generation=rd(u+48,8),
             indicator_attached=rd(u+12296,4),indicator_frames=rd(u+12300,4),indicator_error=rd(u+12312,4))
    engine=rd(u+8,8);s['engine']=hex(engine)
    if engine<0xa00000000 or engine>=0xa40000000:return s
    blob=g.read_bytes(engine,OFFSETS['struct_bytes']);runtime={}
    wide={'poll_count','full_presses','writer_returns','jpeg_returns','capture.irq_seq','capture.copied','capture.dropped','capture.accepted_presses','encode_started_us','encode_finished_us','frame_encode_started_us','frame_encode_finished_us'}
    for name,offset in OFFSETS.items():
        if name=='struct_bytes':continue
        if name in ('last_native_path','debug_ticket_path','debug_dir_path'):
            length=128 if name=='debug_dir_path' else 32
            runtime[name]=blob[offset:offset+length].split(b'\0')[0].decode('ascii','replace')
        else:
            stream32={'width','height','allocation_requested','bitrate','idr_interval_us','job','frozen_count','frozen_size','last_duration_us','error','cleanup_failed','mux_started','mux_error'}
            width=8 if name in wide or (name.startswith('stream.') and name.split('.',1)[1] not in stream32) or name=='capture.press_us' or name=='capture.rejected_presses' else 4
            runtime[name]=int.from_bytes(blob[offset:offset+width],'little')
    s['runtime']=runtime
    return s
class Guest(probe.PreparedGuest):
    def __init__(self,*args,**kw):
        super().__init__(*args,**kw)
        # PreparedGuest validates stock identity/profile. Execute our exact MAIN.
        self.command[self.command.index('-kernel')+1]=str(A.main.resolve())
        at=self.command.index('-icount')+1
        self.command[at]=self.command[at].replace('sleep=on','sleep=off')
        self.data['live_a']=dict(mode=A.mode,prepared_stock_sha256=sha(STOCK),
            executed_main_sha256=sha(A.main),executed_main=str(A.main.resolve()),flashable=False)
        if False:
            syms=subprocess.check_output(['nm',str(A.qemu)],text=True).splitlines()
            def sym(name):return next(int(x.split()[0],16) for x in syms if x.endswith(' '+name))
            plugin=TASK/'alloc_probe.so'
            self.command += ['-plugin',f'{plugin},out={self.out}/allocator-events.dat,clock={sym("timers_state"):#x},cpu={sym("qemu_get_cpu"):#x}']
        self.data['command']=self.command
        print('EXECUTED_MAIN',json.dumps(self.data['live_a']),flush=True)
        for p in [Path(__file__),HERE/'pipe_transport.py',HERE/'card_export.py',HERE/'z8_capture_resources.py',OFFSET_PATH]:
            shutil.copy2(p,self.out/p.name)
    def __enter__(self):
        if A.preflight:
            from z8_resources import attach
            attach(self,native_sampling=True,battery_limits=True)
            (self.out/'preflight.json').write_text(json.dumps(self.data,indent=2)+'\n')
            print('PREFLIGHT_OK: complete prepared guest; no QEMU process launched',flush=True)
            raise PreflightComplete()
        g=super().__enter__();original=g.qmp.command
        def watchdog():
            deadline=time.monotonic()+A.seconds+30
            while g.proc.poll() is None:
                if (g.out/'stop.request').exists() or time.monotonic()>deadline:g.proc.terminate();return
                time.sleep(1)
        threading.Thread(target=watchdog,daemon=True).start()
        self.next_space_check=0
        def command(name,**kw):
            if time.monotonic()>self.next_space_check:
                self.next_space_check=time.monotonic()+5
                if shutil.disk_usage('/mnt/c').free<30*1024**3:raise RuntimeError('C: below 30 GiB')
            result=original(name,**kw)
            if name=='screendump':
                path=Path(kw['filename']);Image.open(path).save(path.with_suffix('.png'));path.unlink()
                info=snapshot(g,path.stem)
                adaptive_contact(g,path.stem,info)
                for prop in ['runtime-clock-state','engine1548-scanout-state','raw-pipeline-state','engine15e2-jpeg-state']:
                    try:info[prop]=json.loads(original('qom-get',path='/machine',property=prop))
                    except RuntimeError as e:info[prop]=str(e)
                g.data.setdefault('live_checkpoints',[]).append(info)
                with (g.out/'live-checkpoints.jsonl').open('a') as f:f.write(json.dumps(info)+'\n')
                print('CHECKPOINT',json.dumps(info),flush=True)
                if path.stem.endswith('-photo-live-10s') and not QUALITY_SELECTED:
                    raise RuntimeError('JPEG fine menu selection was not reached')
                if A.mode!='off' and path.stem.endswith('-photo-live-10s') and info.get('runtime',{}).get('requested')!=1:
                    raise RuntimeError('Actual menu navigation did not enable Live; no On-shot claim allowed')
            return result
        g.qmp.command=command
        return g
probe.PreparedGuest=Guest
class Peer(probe.CapturePeer):
    def __init__(self,*args,**kw):super().__init__(*args,**kw);self.trace_points={}
probe.CapturePeer=Peer
probe.MENU_CONTACTS['ok']=(0x2a,0x8000,0)
probe.MENU_CONTACTS['playback']=(0x44,0,0x2)
def contacts(args):
    global LIVE_CONTACTS
    rows=[]
    def shutter(label,half=False,full=False,gap=1):
        rows.append(dict(label=label,body=probe.shutter_body(half,full),expected=probe.expected_keys(half,full),
                         gap=int(gap*1e9),half=half,full=full))
    def key(label,key,gap=.5):
        body,expected=probe.navigation_contact(key)
        rows.append(dict(label=label,body=body,expected=expected,gap=400000000,half=False,full=False))
        rows.append(dict(label=label+'-release',body=probe.key_body(),expected=probe.expected_keys(),
                         gap=int(gap*1e9),half=False,full=False))
    shutter('wake-half',True);shutter('wake-release')
    # Same actual native JPEG fine selection for baseline, OFF, ON and double.
    key('quality-menu','menu',10);key('quality-left','left');key('quality-right','right')
    for i in range(18):key(f'quality-seek-{i:02}','down')
    # Leave quality child safely, then re-open menu in its retained PHOTO tab.
    shutter('quality-exit-half',True);shutter('quality-exit-release')
    if A.mode!='off':
        key('ui-menu','menu',10);key('ui-left','left')
        # Real MCU arrows chosen from read-only active page/row state.
        # The default screenshot puts PHOTO above MOVIE/CUSTOM/PLAYBACK/SETUP,
        # but seek does not assume how many tab steps are needed.
        for i in range(7):key(f'tab-seek-{i:02}','down')
        for i in range(20):key(f'live-seek-{i:02}','up')
        key('ui-exit-menu','menu',10)
    if A.mode!='off':
        shutter('lv-warmup-half',True,gap=1)
        shutter('lv-warmup-release',gap=6)
    shutter('photo-live-10s',gap=10)
    start=len(rows)
    shutter('half-press',True,gap=2)
    shutter('full-press',True,True,gap=.15)
    shutter('full-release',gap=1.35 if A.mode=='two' else .15)
    if A.mode=='two':
        shutter('second-half-press',True,gap=.5)
        shutter('second-full-press',True,True,gap=.15)
        shutter('second-full-release',gap=.15)
    shutter('post-photo-12s',gap=12)
    shutter('post-encode-30s',gap=30)
    shutter('post-complete',gap=30)
    LIVE_CONTACTS=rows
    return rows,start
probe.contact_sequence=contacts
recipe=json.loads((SNAP/'bundle.json').read_text())['recipe']
names={s.split('=')[0] for s in probe.load_model_profile()['properties']}
names|={'pcie-link-diagnostic-mask','pcie-dbi-model','pcie-ram-dma-input','pcie-image-ram-dma-input','pcie-intx-model','sensor-frame-tag-model'}
extra=[s for s in recipe['properties'] if s.split('=')[0] not in names and ':' not in s]
extra+=['z8-serial5-config-model=on']
profile=RUNROOT/(A.name+'-properties.json')
profile.write_text(json.dumps(dict(model='Nikon Z8',main_sha256=sha(STOCK),properties=extra),indent=2)+'\n')
args=SimpleNamespace(firmware=STOCK,qemu=A.qemu,raw=BASE/'runs/raw-precapture-20261012/input14.raw',output=OUT,seconds=A.seconds,
    settle=1,half_seconds=2,capture_properties=profile,card=True,nvme_trace=False,gpio=True,rtc_status=True,
    tagged_input=True,capture_selector=True,capture_timing_input=True,hsfc_processing_input=False,menu_actions='',menu_only=True,
    wait_liveview=False,navigation_press_ms=400,navigation_release_ms=500,capture_pixels=False,wait_capture_work=False,
    skip_cseq_repeat=True,native_writer_observer=False)
class PreflightComplete(Exception):pass
try:probe.run(args)
except PreflightComplete:pass
finally:
    if OUT.exists():
        for p in OUT.glob('*.ppm'):Image.open(p).save(p.with_suffix('.png'));p.unlink()
        manifest={str(p):sha(p) for p in [STOCK,A.main,A.qemu,Path(__file__),HERE/'z8_capture_resources.py',OFFSET_PATH]}
        (OUT/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
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
