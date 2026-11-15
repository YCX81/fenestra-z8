"""Diagnostic: re-encode each current JPEG in native 4:2:2, never donor pixels."""
import io,json,subprocess,base64
import ncdt_variants as v
R=v.R;src=R/'native-core-all.MOV';d=src.read_bytes();ch=v.ncdt(d);items=[]
for tag in [b'NCTH',b'NCVW',b'NCM1',b'NCM2']:
 script='import io,base64,json\nfrom PIL import Image\nb='+repr(base64.b64encode(ch[tag]).decode())+'\ni=Image.open(io.BytesIO(base64.b64decode(b))).convert("RGB");o=io.BytesIO();i.save(o,format="JPEG",quality=90,subsampling=1,optimize=True)\nprint(json.dumps(dict(size=i.size,data=base64.b64encode(o.getvalue()).decode())))\n'
 result=subprocess.run(['ssh','win-lab','wsl','-d','Ubuntu-26.04','--','/path/to/user/.local/share/nikon-z8-qemu/venv/bin/python','-'],input=script,text=True,capture_output=True,check=True)
 encoded=json.loads(result.stdout);j=base64.b64decode(encoded['data']);d=v.replace(d,[b'moov',b'udta',b'NCDT',tag],j)
 items.append(dict(tag=tag.decode(),dimensions=encoded['size'],input_sha256=v.sh(ch[tag]),output_sha256=v.sh(j)))
p=R/'native-coreall-422.MOV';p.write_bytes(d)
r=dict(source=str(src),source_sha256=v.sh(src.read_bytes()),output_sha256=v.sh(d),bytes=len(d),images=items,scope='current-image JPEGs recompressed 4:2:2, no donor pixels; other boxes preserved')
p.with_suffix('.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r,indent=2))
