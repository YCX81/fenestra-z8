"""One final-batch run; actual menu inputs, stock preparation, pinned overlay kernel.
No guest RAM writes; passive public-API plugin; no Live readiness manipulation. Emulation only.
"""
import argparse,hashlib,json,os,shutil,struct,subprocess,sys,time,threading
from pathlib import Path
from types import SimpleNamespace
HERE=Path(__file__).resolve().parent
TASK=HERE.parent
NIKON=Path.home()/'Nikon'
SNAP=Path.home()/'.local/share/nikon-z8-qemu/live-regress/snap'
ROOT=SNAP/'project'
BASE=Path.home()/'.local/share/nikon-z8-qemu'
RUNROOT=BASE/'live-audio/regress/runs'
STOCK=NIKON/'z8-qemu/.local/firmware/z8-c311-main.bin'
PY=BASE/'venv/bin/python'
P=argparse.ArgumentParser()
P.add_argument('--name',required=True)
P.add_argument('--main',type=Path,required=True)
P.add_argument('--mode',choices=['off','on','two','double','pb'],required=True)
P.add_argument('--seconds',type=int,default=900)
P.add_argument('--qemu',type=Path,default=BASE/'qemu-port/build/qemu-system-aarch64')
P.add_argument('--retry-offsets',action='store_true')
P.add_argument('--preflight',action='store_true',help='Construct and validate complete guest command/resources, never launch QEMU')
P.add_argument('--keep-large',action='store_true',help='Debug-only opt-out from normal finished-run cleanup')
A=P.parse_args()
if not A.name or Path(A.name).name!=A.name:raise ValueError('single run-directory name required')
if not 30<=A.seconds<=900:raise ValueError('host deadline 30..900 seconds required')
print(subprocess.check_output(['df','-h',str(Path.home())],text=True),flush=True)
if shutil.disk_usage(str(Path.home())).free<30*1024**3:raise RuntimeError('C: below 30 GiB')
meminfo=dict(line.split(':',1) for line in Path('/proc/meminfo').read_text().splitlines())
if int(meminfo['MemAvailable'].split()[0])*1024<2.5*1024**3:raise RuntimeError('Less than 2.5 GiB available')
# win-lab: 24 threads, ~0.6 GB RSS per QEMU; limits are tunable per batch.
MIN_GAP=int(os.environ.get('LIVE_QEMU_MIN_GAP_S','90'));MAX_QEMU=int(os.environ.get('LIVE_QEMU_MAX','10'))
active=0
for ent in Path('/proc').iterdir():
    if not ent.name.isdigit():continue
    try:
        cmd=(ent/'cmdline').read_bytes().split(b'\0')[0]
        if b'qemu' not in Path(os.fsdecode(cmd)).name.encode():continue
        active+=1
        elapsed=subprocess.check_output(['ps','-p',ent.name,'-o','etimes='],text=True).strip()
        if elapsed and int(elapsed)<MIN_GAP and not A.preflight:raise RuntimeError(f'QEMU {ent.name} started <{MIN_GAP} seconds ago')
    except (FileNotFoundError,ProcessLookupError,PermissionError):pass
if active>=MAX_QEMU and not A.preflight:raise RuntimeError(f'{MAX_QEMU} QEMU processes already running')
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
def recopy_partition(path,result):
    """Host setup only (win-lab, exfatprogs 1.3.2): some mkfs extents read back as zeros
    during the snapshot's first copy. Copy every allocated extent again and verify."""
    import vrc_extents_helper
    return vrc_extents_helper.recopy(path,result)
def card_create(path,fs='blank'):
    result=_create(path,fs)
    if fs=='exfat':result['recopied_extents']=recopy_partition(path,result)
    if not os.environ.get('NO_FEN'):result['fenestra_card']=exfat_put.put(path,'FENESTRA',{})
    return result
vrc.create=card_create
def _extents_separate_fd(stream,length):
    # Python 3.14: probe holes on a separate fd so the buffered stream's offset stays valid.
    fd=os.open(f'/proc/self/fd/{stream.fileno()}',os.O_RDONLY);position=0
    try:
        while position<length:
            try:start=os.lseek(fd,position,os.SEEK_DATA)
            except OSError:return
            end=min(length,os.lseek(fd,start,os.SEEK_HOLE));yield start,end;position=end
    finally:os.close(fd)
vrc.extents=_extents_separate_fd
OFFSET_PATH=TASK/'runtime-offsets.json'
OFFSETS=json.loads(OFFSET_PATH.read_text())
FIELD_WIDTHS=json.loads((TASK/'runtime-widths.json').read_text())
PB_OFFSETS=json.loads((TASK/'pb-offsets.json').read_text())
PB_WIDTHS=json.loads((TASK/'pb-widths.json').read_text())
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
    global TAB_ENTERED,QUALITY_SELECTED,LIVE_SELECTED
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
    try:
        base=rd(0xa032b50d8,8);fs=rd(base+0xf8a0+0x50,8) if base else 0
        if 0xa00000000<=fs<0xa40000000:
            ptp=fs+0x11730;s.update(fx_state=hex(fs),fx_ptp=hex(ptp),fx_ptp_enabled=rd(ptp,4),fx_ptp_mark=rd(ptp+0x24,4),fx_active48=rd(fs+0x48,4),fx_active54=rd(fs+0x54,4))
        else:s.update(fx_state=hex(fs))
    except Exception as e:s.update(fx_ptp_error=repr(e))
    try:s.update(w5_vdi=hex(rd(0xa02d3d980,8)),w5_vdi58=hex(rd(0xa02d3d980+0x58,8)),w5_client_map=hex(rd(0xa02e98484,4)),w5_worker=hex(rd(0xa02d3274c,4)))
    except Exception as e:s.update(w5_error=repr(e))
    u=rd(state+2528,8);s['ui']=hex(u)
    if 0xa00000000<=u<0xa40000000 and rd(u,4)==0x47525846:  # shared FXRG registry: live_ui at +8
        s['registry']=hex(u);u=rd(u+8,8);s['ui']=hex(u)
    try:
        reg=rd(state+2528,8)
        if 0xa00000000<=reg<0xa40000000 and rd(reg,4)==0x47525846:
            pb=rd(reg+16,8)
            if 0xa00000000<=pb<0xa40000000 and rd(pb,4)==0x50425549:
                cstr=lambda a:g.read_bytes(a,64).split(b'\0',1)[0].decode('latin1')
                s['pb']=dict(started=rd(pb+40,4),current_id=rd(pb+52,4),cache_valid=rd(pb+56,4),lookup_count=rd(pb+64,4),
                    lookup_error=rd(pb+68,4),frames=rd(pb+72,4),playing=rd(pb+76,4),native_path=cstr(pb+352),directory=cstr(pb+416))
                meta=g.hook_symbols['pb_runtime_metadata']
                assert rd(meta,4)==0x50425549 and rd(meta+4,4)==1
                s['pb']['consumed_rec']=rd(pb+rd(meta+33*4,4),4)
                s['pb']['error']=rd(pb+rd(meta+5*4,4),4)
                pb_blob=g.read_bytes(pb,PB_OFFSETS['struct_bytes'])
                for field,width in PB_WIDTHS.items():
                    at=PB_OFFSETS[field];s['pb'][field]=int.from_bytes(pb_blob[at:at+width],'little')
    except Exception as e:s['pb_error']=repr(e)
    try:s['cardslot']=g.read_bytes(0xa1171b000,72).hex();s['twin']=g.read_bytes(0xa1171b100,48).hex()
    except Exception as e:s['cardslot_error']=repr(e)
    if u<0xa00000000 or u>=0xa40000000:return s
    s.update(ui_magic=hex(rd(u,4)),save_error=rd(u+28,4),loaded=rd(u+32,4),generation=rd(u+48,8),
             indicator_attached=rd(u+12296,4),indicator_frames=rd(u+12300,4),indicator_error=rd(u+12312,4))
    engine=rd(u+8,8);s['engine']=hex(engine)
    if engine<0xa00000000 or engine>=0xa40000000:return s
    blob=g.read_bytes(engine,OFFSETS['struct_bytes']);runtime={}
    wide={'poll_count','full_presses','writer_returns','jpeg_returns','capture.irq_seq','capture.copied','capture.dropped','capture.accepted_presses','encode_started_us','encode_finished_us','frame_encode_started_us','frame_encode_finished_us','capture.irq_timestamp'}
    for name,offset in OFFSETS.items():
        if name=='struct_bytes':continue
        if name in ('last_native_path','debug_ticket_path','debug_dir_path'):
            length=128 if name=='debug_dir_path' else 32
            runtime[name]=blob[offset:offset+length].split(b'\0')[0].decode('ascii','replace')
        else:
            stream32={'width','height','allocation_requested','bitrate','idr_interval_us','job','frozen_count','frozen_size','last_duration_us','error','cleanup_failed','mux_started','mux_error'}
            width=8 if name in wide or (name.startswith('stream.') and name.split('.',1)[1] not in stream32) or name=='capture.press_us' or name=='capture.rejected_presses' else 4
            width=FIELD_WIDTHS[name]
            runtime[name]=int.from_bytes(blob[offset:offset+width],'little')
    assert runtime['size']==OFFSETS['struct_bytes'], 'Runtime ABI size disagrees with this exact AArch64 source layout'
    s['runtime']=runtime
    if '-pb-' in label or label=='final':
        hw=runtime.get('stream.encoder.hw',0)
        if not hw:
            s['w5_close_diag']={'hw':0}
        elif 0x800000000<=hw<=0xa40000000-1057024 and hw%16==0:
            head=g.read_bytes(hw,64);tail=g.read_bytes(hw+1053528,88)
            diag={'hw':hex(hw),'head':head.hex(),'tail':tail.hex(),'client_map':g.read_bytes(0xa02e98480,5).hex(),'arena':hex(rd(0xa02d3d950,8)),'partition_b0':hex(rd(0xa02d3d9d8,8)),'native_instance_binding':hex(rd(hw+1057016,8))}
            for field,offset in [('stream',4),('frames',28),('busy',32),('release_req',36)]:diag[field]=int.from_bytes(head[offset:offset+4],'little')
            for field,offset in [('closes',12),('eos_ok',16),('eos_miss',20),('fin_rc',24),('last_close_us',28),('close_failed',36)]:diag[field]=int.from_bytes(tail[offset:offset+4],'little')
            stream=diag['stream']
            if stream<=1:
                diag.update(slot_used=rd(0xa02d30118+stream*12,4),worker=rd(0xa02d3274c+stream*4,4))
                inst=rd(0xa02d32758+stream*8,8);diag['instance']=hex(inst)
                if 0x800000000<=inst<=0xa40000000-16 and inst%4==0:diag['instance_head']=g.read_bytes(inst,16).hex()
            s['w5_close_diag']=diag
    if label=='final':
        # Only AFTER the final assertion; no per-contact reads/timing perturbation.
        s['stock_error_history']={'address':'0xa03e04f44','hex':g.read_bytes(0xa03e04f44,0xa0).hex(),'layout':'five 32-byte ErrorInformationService history records; raw evidence'}
    return s
class Guest(probe.PreparedGuest):
    def __init__(self,*args,**kw):
        super().__init__(*args,**kw)
        # PreparedGuest validates stock identity/profile. Execute our exact MAIN.
        self.command[self.command.index('-kernel')+1]=str(A.main.resolve())
        model_property='z8-heif-secondary-stop-model=on'
        machine_index=self.command.index('-M')+1
        assert 'z8-heif-secondary-stop-model=' not in self.command[machine_index]
        self.command[machine_index]+=','+model_property
        self.data['machine_properties'].append(model_property)
        self.data['a2_model_comparison']={'only_functional_change':model_property,'firmware_and_deadlines_unchanged':True,'existing_model_strict_transaction_gates':True}
        at=self.command.index('-icount')+1
        self.command[at]=self.command[at].replace('sleep=on','sleep=off')
        self.data['live_a']=dict(mode=A.mode,prepared_stock_sha256=sha(STOCK),
            executed_main_sha256=sha(A.main),executed_main=str(A.main.resolve()),flashable=False)
        import lv_diagnostics
        manifest=json.loads((A.main.parent/'changed-ranges.json').read_text())
        self.hook_symbols=manifest['symbols']
        points={100:'live_irq_notify',101:'live_epoch_notify',102:'live_press_notify',103:'live_writer_wrap',104:'live_encoder_frame_timed'}
        plugin=TASK/'lv_observer.so'
        options=','.join(f'point={k}:{self.hook_symbols[v]:#x}' for k,v in points.items())
        error_points={200: 1089268028, 201: 1090620032, 202: 1089843872, 203: 1089811132, 204: 1091916560, 205: 1091916852, 206: 1091916856, 207: 1091916892, 208: 1091903636, 209: 1091903680, 210: 1091903556, 211: 1091915192, 212: 1091916528, 220: 1091926320, 221: 1091929964, 222: 1091935016, 223: 1091942412, 224: 1091942496, 225: 1091943104, 226: 1091943248, 227: 1091943340, 228: 1091946276, 229: 1091948316, 230: 1091951336, 231: 1091953192, 232: 1091954332, 233: 1091956452, 234: 1091959780, 235: 1091971980, 236: 1091972036, 237: 1091972332, 240: 1091959740, 241: 1091959744, 242: 1091902712, 243: 1091903192, 244: 1091902044, 245: 1091959544}
        options+=','+','.join(f'point={kind}:{pc:#x}' for kind,pc in error_points.items())
        self.command += ['-plugin',f'{plugin},out={self.out}/lv-observer.dat,{options}']
        self.data['stock_error_observer']={'points':{str(k):hex(v) for k,v in error_points.items()},'public_plugin_api':True,'guest_writes':False,'event_clock':'host monotonic only'}
        self.data['lv_observer']=dict(plugin=str(plugin),sha256=sha(plugin),points=points,host_clock_only=True,guest_writes=False)
        self.diagnostic_previous={}
        self.data['command']=self.command
        print('EXECUTED_MAIN',json.dumps(self.data['live_a']),flush=True)
        for p in [Path(__file__),HERE/'pipe_transport.py',HERE/'card_export.py',HERE/'z8_capture_resources.py',HERE/'lv_diagnostics.py',TASK/'lv_observer.c',OFFSET_PATH]:
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
                if shutil.disk_usage(str(Path.home())).free<30*1024**3:raise RuntimeError('C: below 30 GiB')
            result=original(name,**kw)
            if name=='screendump':
                path=Path(kw['filename']);Image.open(path).save(path.with_suffix('.png'));path.unlink()
                info=snapshot(g,path.stem)
                adaptive_contact(g,path.stem,info)
                if path.stem.endswith('-pb-return-half'):
                    pb=info.get('pb',{});r=info.get('runtime',{})
                    assert pb.get('frames',0)>0 and pb.get('playing')==0 and pb.get('error')==0 and pb.get('lookup_error')==0, 'Preview did not complete inside bounded wait'
                    assert g.data.get('rec_owned_expectation',{}).get('after') is not None, 'REC ownership was not observed'
                    g.data['pb_recovery_before']={'irq':r.get('capture.irq_seq',0),'encoded':r.get('stream.encoded',0),'preview_frames':pb['frames']}
                if path.stem.endswith('-pb-recovered'):
                    before=g.data['pb_recovery_before'];r=info.get('runtime',{})
                    assert info.get('photo_movie')==0 and r.get('requested')==1, 'Live did not become requested in photo mode'
                    assert r.get('capture.irq_seq',0)>before['irq'] and r.get('stream.encoded',0)>before['encoded'], 'No new Live frames/encoding after half-press return'
                    assert r.get('stream.error')==0 and r.get('stream.job')==0, 'Recovered Live stream has an error or unfinished clip'
                    g.data['pb_recovery_after']={'irq':r['capture.irq_seq'],'encoded':r['stream.encoded'],'phase':r.get('capture.phase'),'preview_frames':info['pb']['frames']}
                if path.stem=='final':
                    # Final only, after all original recovery assertions. No guest writes.
                    info['a2_model_final']={}
                    for prop in ['z8-heif-secondary-stop-state','event1508-global1-state','event1508-channel-state','event1508-group-state']:
                        try:info['a2_model_final'][prop]=json.loads(original('qom-get',path='/machine',property=prop))
                        except RuntimeError as e:info['a2_model_final'][prop]=str(e)
                for prop in ['runtime-clock-state','engine1548-scanout-state','raw-pipeline-state','engine15e2-jpeg-state']:
                    try:info[prop]=json.loads(original('qom-get',path='/machine',property=prop))
                    except RuntimeError as e:info[prop]=str(e)
                import lv_diagnostics
                info['lv_diagnostics']=lv_diagnostics.capture(g,original,info,self.diagnostic_previous,self.hook_symbols)
                g.data.setdefault('live_checkpoints',[]).append(info)
                with (g.out/'live-checkpoints.jsonl').open('a') as f:f.write(json.dumps(info)+'\n')
                print('CHECKPOINT',json.dumps(info),flush=True)
                if path.stem.endswith('-photo-live-10s') and not QUALITY_SELECTED:
                    raise RuntimeError('JPEG fine menu selection was not reached')
                if path.stem.endswith('-photo-live-10s') and (not LIVE_SELECTED or info.get('runtime',{}).get('requested')!=(0 if A.mode=='off' else 1)):
                    raise RuntimeError('Actual menu navigation did not enable Live; no On-shot claim allowed')
            return result
        g.qmp.command=command
        return g
probe.PreparedGuest=Guest
class Peer(probe.CapturePeer):
    def __init__(self,*args,**kw):
        super().__init__(*args,**kw);self.trace_points={};self.rec_before=None
    def begin(self,label,body,expected):
        if label=='pb-play-trigger':
            info=snapshot(self.g,'rec-ownership-before')
            pb=info.get('pb',{})
            assert pb.get('cache_valid')==1 and pb.get('lookup_error')==0 and pb.get('native_path'), 'No verified Live photo owner for REC'
            assert body[9]==1 and int.from_bytes(expected[4:8],'little')==0x2000
            self.rec_before=pb['consumed_rec']
            # pb_key_decode_wrap deliberately consumes REC before original key consumer.
            # Retain an exact 44-byte check and verify the owning hook counter on return.
            expected=expected[:4]+bytes(4)+expected[8:]
            self.g.data['rec_owned_expectation']={'before':self.rec_before,'expected_hex':expected.hex(),'body_hex':body.hex()}
        return super().begin(label,body,expected)
    def handle(self):
        active=self.active_contact
        result=super().handle()
        if active and active['label']=='pb-play-trigger' and self.active_contact is None:
            info=snapshot(self.g,'rec-ownership-after');pb=info.get('pb',{})
            assert active['entered'] and pb.get('consumed_rec',0)==self.rec_before+1, 'REC was not consumed exactly once by playback owner'
            self.g.data['rec_owned_expectation']['after']=pb['consumed_rec']
        return result
probe.CapturePeer=Peer
probe.MENU_CONTACTS['ok']=(0x2a,0x8000,0)
probe.MENU_CONTACTS['playback']=(0x44,0,0x2)
probe.MENU_CONTACTS['ok']=(0x2a,0x8000,0)
probe.MENU_CONTACTS['rec']=(0x09,0,0x2000)
probe.MENU_CONTACTS['delete']=(0x45,0,0x4000)
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
    if True:
        key('ui-menu','menu',10);key('ui-left','left')
        # Real MCU arrows chosen from read-only active page/row state.
        # The default screenshot puts PHOTO above MOVIE/CUSTOM/PLAYBACK/SETUP,
        # but seek does not assume how many tab steps are needed.
        for i in range(7):key(f'tab-seek-{i:02}','down')
        for i in range(20):key(f'live-seek-{i:02}','up')
        key('ui-exit-menu','menu',10)
    shutter('photo-live-10s',gap=10)
    start=len(rows)
    shutter('half-press',True,gap=2)
    shutter('full-press',True,True,gap=.15)
    # two: full presses 2.0 s apart; double: 0.33 s apart (.15+.03+.15).
    shutter('full-release',gap={'two':1.35,'double':.03}.get(A.mode,.15))
    if A.mode in ('two','double'):
        shutter('second-half-press',True,gap=.5 if A.mode=='two' else .15)
        shutter('second-full-press',True,True,gap=.15)
        shutter('second-full-release',gap=.15)
    shutter('post-photo-12s',gap=12)
    if A.mode=='pb':   # enter playback before the standby timer (QEMU idled out after ~60 s)
        shutter('post-encode-8s',gap=8)
        shutter('post-complete',gap=1)
    else:
        shutter('post-encode-30s',gap=30)
        shutter('post-complete',gap=30)
    if A.mode=='pb':
        key('pb-enter','playback',5)
        shutter('pb-view',gap=2)
        key('pb-play-trigger','rec',.25)
        shutter('pb-play-frame-a',gap=.5)
        shutter('pb-play-frame-b',gap=.75)
        shutter('pb-play-end',gap=3)
        shutter('pb-completion-wait',gap=8)
        shutter('pb-return-half',True,gap=.5)
        shutter('pb-return-release',gap=10)
        shutter('pb-recovered',gap=2)
    LIVE_CONTACTS=rows
    return rows,start
probe.contact_sequence=contacts
recipe=json.loads((SNAP/'bundle.json').read_text())['recipe']
names={s.split('=')[0] for s in probe.load_model_profile()['properties']}
names|={'pcie-link-diagnostic-mask','pcie-dbi-model','pcie-ram-dma-input','pcie-image-ram-dma-input','pcie-intx-model','sensor-frame-tag-model'}
extra=[s for s in recipe['properties'] if s.split('=')[0] not in names and ':' not in s]
extra+=['z8-serial5-config-model=on','engine153-alternate-bank-model=on','z8-jpeg-decode=on']
# Wave5 encoder model on by default (Live clips need it); W5_OFF=1 restores the old stop-at-Wave5 behaviour.
if not os.environ.get('W5_OFF') and 'wave5-upstream-model=on' not in extra:extra.append('wave5-upstream-model=on')
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
