from pathlib import Path
import shutil,json,hashlib,ast,py_compile
H=Path.home();D=H/'fxfix/v50-playback/recovery-secondary-stop-driver'
assert not D.exists()
shutil.copytree(H/'fxfix/v50-playback/recovery-a2-wait-driver',D)
p=D/'retry-driver/run_live.py';s=p.read_text();before=hashlib.sha256(s.encode()).hexdigest()
line=next(l for l in s.splitlines() if l.startswith('        error_points='))
points=ast.literal_eval(line.split('=',1)[1])
points.update({240:0x4115fbbc,241:0x4115fbc0,242:0x41151cf8,243:0x41151ed8,244:0x41151a5c,245:0x4115faf8})
assert 18+5+len(points)<=64
s=s.replace(line,'        error_points='+repr(points))
a="        at=self.command.index('-icount')+1"
b="""        model_property='z8-heif-secondary-stop-model=on'
        machine_index=self.command.index('-M')+1
        assert 'z8-heif-secondary-stop-model=' not in self.command[machine_index]
        self.command[machine_index]+=','+model_property
        self.data['machine_properties'].append(model_property)
        self.data['a2_model_comparison']={'only_functional_change':model_property,'firmware_and_deadlines_unchanged':True,'existing_model_strict_transaction_gates':True}
"""+a
assert s.count(a)==1;s=s.replace(a,b)
a="                for prop in ['runtime-clock-state','engine1548-scanout-state','raw-pipeline-state','engine15e2-jpeg-state']:"
b="""                if path.stem=='final':
                    # Final only, after all original recovery assertions. No guest writes.
                    info['a2_model_final']={}
                    for prop in ['z8-heif-secondary-stop-state','event1508-global1-state','event1508-channel-state','event1508-group-state']:
                        try:info['a2_model_final'][prop]=json.loads(original('qom-get',path='/machine',property=prop))
                        except RuntimeError as e:info['a2_model_final'][prop]=str(e)
"""+a
assert s.count(a)==1;s=s.replace(a,b);p.write_text(s)
py_compile.compile(str(p),doraise=True)
provenance={'parent_driver_sha256':before,'driver_sha256':hashlib.sha256(p.read_bytes()).hexdigest(),
 'functional_change':'existing z8-heif-secondary-stop-model=on only',
 'observations_added':{str(k):hex(points[k]) for k in range(240,246)},
 'final_only_qom_reads':True,'firmware_deadlines_inputs_assertions_unchanged':True}
(D/'secondary-stop-provenance.json').write_text(json.dumps(provenance,indent=2)+'\n');print(json.dumps(provenance,indent=2))
