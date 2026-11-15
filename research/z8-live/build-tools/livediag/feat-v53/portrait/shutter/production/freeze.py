from pathlib import Path
import hashlib,json,difflib
P=Path(__file__).resolve().parent;W=P/'work';B=P.parents[3]/'feat-v52/integration-g/work/src-fix37';files=['runtime.c','runtime.h','audio/live_audio.c','audio/live_audio.h','audio/live_audio_z8.c','audio/live_audio_z8.h','audio/live_shutter_z8.c','audio/live_shutter_z8.h','codec/live_w5enc.c']
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest() if p.exists()else None
rows=[];patch=''
for name in files:
 a=B/name;b=W/name;rows.append(dict(path='src-fix37/'+name,old_sha256=sha(a),new_sha256=sha(b)))
 patch+=''.join(difflib.unified_diff(a.read_text().splitlines(True)if a.exists()else [],b.read_text().splitlines(True),fromfile='a/src-fix37/'+name if a.exists()else '/dev/null',tofile='b/src-fix37/'+name))
(P/'shutter-production.patch').write_text(patch)
r=dict(baseline=str(B),candidate=str(W),includes_audio_startup_patch='f3465b009d14b134297298945959abbaf7ff1f690a85cdbc5a95b2bac4f0894e4c4878674daa',files=rows,patch_sha256=sha(P/'shutter-production.patch'),provisional_review=True)
r['includes_audio_startup_patch']=hashlib.sha256((P.parents[2]/'audio/capture-startup.patch').read_bytes()).hexdigest()
(P/'changed-files.json').write_text(json.dumps(r,indent=2)+'\n')
h=dict(schema='v53-shutter-hook/v1',input_native_image_sha256='ed1e8496fccde443412e527a252de5029a55c749053b9b046da42bfc454aa613',patches=[dict(va='0x40e30e3c',expected_hex='3c0f0014',operation='AArch64 B',target_symbol='live_shutter_dispatch',require_alignment=4,why='Serialized original SFX task manager tail dispatch; original 40e34b2c remains callable. Keep symbol in linked .live_shutter range and assert signed branch immediate.')],ram_abi=json.loads((P/'size.json').read_text())['abi'],bank=dict(section='.live_shutter',proposed_start='0x4019f020',limit='0x4019ff40',actual_object_bytes=json.loads((P/'size.json').read_text())['delta']['shutter']),hardware_verified=False)
(P/'hook-manifest.json').write_text(json.dumps(h,indent=2)+'\n');print(r['patch_sha256'])
