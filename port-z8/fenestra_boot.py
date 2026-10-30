"""QEMU bring-up of the Fenestra Z8 candidate MAIN: boot, open the menu, walk tabs, report install state.

Simulation only. Reads the Setup page descriptor through the page root to see whether the core installed.
Usage: fenestra_boot.py --name RUN --actions menu,left,down,...
"""
import argparse, hashlib, json, os, struct, sys, time
from pathlib import Path
from types import SimpleNamespace

NIKON=Path('/mnt/c/Users/ycx/Desktop/Nikon')
ROOT=NIKON/'z8-he-history-20261017'
sys.path.insert(0,str(ROOT/'tools'))
import z8_capture_probe as probe
from PIL import Image

P=argparse.ArgumentParser();P.add_argument('--name',required=True);P.add_argument('--main',required=True)
P.add_argument('--actions',default='menu,left');P.add_argument('--seconds',type=int,default=900)
P.add_argument('--tail-s',type=int,default=6)
P.add_argument('--module',action='append',default=[])
P.add_argument('--no-dir',action='store_true')
A=P.parse_args()
BASE=Path('/home/ycx/.local/share/nikon-z8-qemu')
OUT=BASE/'fenestra-z8'/A.name
OUT.parent.mkdir(parents=True,exist_ok=True)
STOCK=NIKON/'z8-integration-20261014/.local/firmware/z8-c311-main.bin'
QEMU=BASE/'lossless-c30-20261018/bin/qemu-i6i-he03-01'
RAW=BASE/'runs/raw-precapture-20261012/input14.raw'
MAIN=Path(A.main)
sha=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest()
SCR=int(os.environ['FX_SCR'],16)
ROOTS,SETUP,STOCK_DESC=0xa032b50c8,663,0xa028ca040

def install_state(g):
    rd=lambda a,n:int.from_bytes(g.read_bytes(a,n),'little')
    root=rd(ROOTS+2*8,8)
    bc=dict(events=rd(SCR,4),ev={e:rd(SCR+8+4*e,4) for e in (7,19,20,24)},act=[rd(SCR+4*i,4) for i in range(70,80)])
    if not root:return dict(root=0)
    d=rd(root+SETUP*96+80,8)
    out=dict(bc=bc,root=hex(root),descriptor=hex(d),installed=d!=STOCK_DESC)
    if d!=STOCK_DESC:
        out.update(magic=hex(rd(d+64,4)),state_installed=rd(d+68,4),activation=rd(d+72,4),phase=rd(d+76,4),error=rd(d+80,4),count=rd(d+32,2))
    return out

Parent=probe.PreparedGuest
class FenestraGuest(Parent):
    def __enter__(self):
        g=super().__enter__();command=g.qmp.command
        def observed(name,**kw):
            result=command(name,**kw)
            if name=='screendump':
                path=Path(kw['filename'])
                try:Image.open(path).save(path.with_suffix('.png'))
                except Exception as e:print('png',e,flush=True)
                st=install_state(g);g.data.setdefault('fenestra_state',[]).append(dict(label=path.stem,state=st))
                print(path.stem+': '+json.dumps(st),flush=True)
            return result
        g.qmp.command=observed
        return g
    def __init__(self,*args,**kwargs):
        super().__init__(*args,**kwargs)
        self.command[self.command.index('-kernel')+1]=str(MAIN)
        self.data['fenestra']=dict(source_main_sha256=sha(STOCK),executed_main_sha256=sha(MAIN),flashable=False)
probe.PreparedGuest=FenestraGuest

probe.MENU_CONTACTS['ok']=(0x2a,0x8000,0)  # MCU key z8_c10_gui.MCU_MENU_KEYS; decoded state observed in fen-12
import virtual_recording_card as _vrc, exfat_put
_create=_vrc.create
def _create_with_dir(path,fs='blank'):
    r=_create(path,fs)
    if not A.no_dir:
        r['fenestra_card']=exfat_put.put(path,'FENESTRA',{Path(m).name:Path(m).read_bytes() for m in A.module})
    return r
_vrc.create=_create_with_dir
ACTIONS=[x for x in A.actions.split(',') if x]
def sequence(args):
    rows=[]
    def shutter(label,half,gap=1000000000):
        rows.append(dict(label=label,body=probe.shutter_body(half,False),expected=probe.expected_keys(half,False),gap=gap,half=half,full=False))
    shutter('wake-half',True);shutter('wake-release',False)
    for i,action in enumerate(ACTIONS):
        body,expected=probe.navigation_contact(action)
        rows.append(dict(label=f'nav-{i}-{action}',body=body,expected=expected,gap=400000000,half=False,full=False))
        rows.append(dict(label=f'nav-{i}-release',body=probe.key_body(),expected=probe.expected_keys(),
                         gap=(10000 if action=='menu' else 1500)*1000000,half=False,full=False))
    shutter('end-half',False,gap=A.tail_s*1000000000);shutter('end-tail',False)
    return rows,len(rows)
probe.contact_sequence=sequence

BaseWritePeer=probe.WritePeer
class BoundedWritePeer(BaseWritePeer):
    def __init__(self,g,continuation=False,**kw):
        super().__init__(g,continuation,**kw)
        if self.channel==8 and not continuation:
            self.record.update(limit_batches=4096,limit_words=65536)
probe.WritePeer=BoundedWritePeer

profile=json.loads((ROOT/'profiles/z8-capture-properties.json').read_text())
props=OUT.parent/(A.name+'-properties.json')
if props.exists():raise FileExistsError(props)
props.write_text(json.dumps(profile,indent=2)+'\n')
args=SimpleNamespace(firmware=STOCK,qemu=QEMU,raw=RAW,output=OUT,seconds=A.seconds,settle=10,half_seconds=2,
    capture_properties=props,card=True,nvme_trace=False,gpio=True,rtc_status=True,tagged_input=True,
    capture_selector=True,capture_timing_input=False,hsfc_processing_input=False,menu_actions='',menu_only=True,
    wait_liveview=False,navigation_press_ms=400,navigation_release_ms=500,capture_pixels=False,
    wait_capture_work=False,skip_cseq_repeat=True,native_writer_observer=False)
try:probe.run(args)
except BaseException as e:
    print(type(e).__name__+': '+str(e),flush=True);raise
