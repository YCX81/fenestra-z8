"""Rewrite SOURCE-INVENTORY.json from the git index (LF content), so it does not depend on the checkout's line endings."""
import hashlib, json, subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
git=lambda *a:subprocess.run(['git','-C',str(ROOT),*a],capture_output=True,check=True).stdout
names=sorted(n for n in git('ls-files','-z').decode().split('\0') if n and n!='SOURCE-INVENTORY.json')
rows=[]
for n in names:
    b=git('cat-file','blob',':'+n)
    rows.append(dict(path=n,bytes=len(b),sha256=hashlib.sha256(b).hexdigest()))
old=json.loads((ROOT/'SOURCE-INVENTORY.json').read_text(encoding='utf-8'))
old['files']=rows
(ROOT/'SOURCE-INVENTORY.json').write_text(json.dumps(old,indent=2)+'\n',encoding='utf-8',newline='\n')
print(len(rows))
