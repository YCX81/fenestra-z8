"""The single planned batch; no image mutation or retry inside this driver."""
import datetime,json,os,shutil,subprocess,time
from pathlib import Path
D=Path(__file__).resolve().parent;H=Path('/path/to/user/.local/share/nikon-z8-qemu');R=H/'live-a-driver-20261104';P=H/'integration-20261014/venv/bin/python';Q=H/'live-a-v2-qemu/build/qemu-system-aarch64';B=H/'fenestra-z8/bundle/live-a/v4/live-a.emulation-only.main'
rows=[('v4-retry-on','on'),('v4-retry-two','two')]
ledger=[];running=[];last=0

def save():(D/'batch-v4-retry-results.json').write_text(json.dumps(ledger,indent=2)+'\n')
def reap():
 for p,row,log in running:
  if 'exit_code' not in row and p.poll() is not None:
   row['exit_code']=p.returncode;row['finished_utc']=datetime.datetime.now(datetime.timezone.utc).isoformat();log.close();save();print('FINISH',row['name'],p.returncode,flush=True)
def available():return int(next(x for x in Path('/proc/meminfo').read_text().splitlines() if x.startswith('MemAvailable:')).split()[1])*1024
def processes():
 found=[]
 for ent in Path('/proc').iterdir():
  if not ent.name.isdigit():continue
  try:
   cmd=(ent/'cmdline').read_bytes().split(b'\0')[0]
   if b'qemu' not in Path(os.fsdecode(cmd)).name.encode():continue
   age=int(subprocess.check_output(['ps','-p',ent.name,'-o','etimes='],text=True).strip());found.append((int(ent.name),age))
  except (FileNotFoundError,ProcessLookupError,PermissionError,ValueError):pass
 return found
for name,mode in rows:
 while True:
  reap();ps=processes();mem=available()
  if shutil.disk_usage('/mnt/c').free<30*1024**3:raise RuntimeError('C: below 30 GiB')
  if time.monotonic()-last>=100 and len(ps)<min(10,int(mem/(2.5*1024**3))) and all(age>=90 for _,age in ps) and mem>=2.5*1024**3:break
  time.sleep(2)
 cmd=[str(P),'-u','-B',str(D/'driver/run_live.py'),'--name',name,'--main',str(B),'--mode',mode,'--seconds','900','--qemu',str(Q)]
 log=(R/(name+'.driver.log')).open('w');env=os.environ.copy();env.update(PYTHONDONTWRITEBYTECODE='1',TMPDIR=str(R),FX_ICOUNT_SLEEP='off')
 proc=subprocess.Popen(cmd,stdout=log,stderr=subprocess.STDOUT,env=env,start_new_session=True);last=time.monotonic()
 row=dict(name=name,mode=mode,pid=proc.pid,available_bytes=mem,other_qemu=ps,c_free_bytes=shutil.disk_usage('/mnt/c').free,started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),command=cmd)
 ledger.append(row);running.append((proc,row,log));save();print('START',name,proc.pid,flush=True)
while any(p.poll() is None for p,_,_ in running):reap();time.sleep(2)
reap();print('BATCH_COMPLETE',flush=True)
