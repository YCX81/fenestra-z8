from pathlib import Path
import shutil
H=Path.home();D=H/'fxfix/v50-playback/recovery-error-driver'
assert not D.exists();shutil.copytree(H/'fxfix/v50-playback/recovery-noobserve-driver',D)
p=D/'retry-driver/run_live.py';s=p.read_text()
a="        self.command += ['-plugin',f'{plugin},out={self.out}/lv-observer.dat,{options}']"
b="""        error_points={200:0x40ece93c,201:0x41018a80}
        options+=','+','.join(f'point={kind}:{pc:#x}' for kind,pc in error_points.items())
        self.command += ['-plugin',f'{plugin},out={self.out}/lv-observer.dat,{options}']
        self.data['stock_error_observer']={'points':{str(k):hex(v) for k,v in error_points.items()},'public_plugin_api':True,'guest_writes':False,'event_clock':'host monotonic only'}"""
assert s.count(a)==1;s=s.replace(a,b)
a="    s['runtime']=runtime\n    return s"
b="""    s['runtime']=runtime
    if label=='final':
        # Only AFTER the final assertion; no per-contact reads/timing perturbation.
        s['stock_error_history']={'address':'0xa03e04f44','hex':g.read_bytes(0xa03e04f44,0xa0).hex(),'layout':'five 32-byte ErrorInformationService history records; raw evidence'}
    return s"""
assert s.count(a)==1;s=s.replace(a,b)
p.write_text(s);print(p)
