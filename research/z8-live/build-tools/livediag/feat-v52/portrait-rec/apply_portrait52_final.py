"""Strict combined LCD+EVF overlay; apply directly to a pre-portrait52 MAIN.
All affected 68B records must equal v51d; unrelated linked changes are allowed.
"""
import argparse,json,struct
from pathlib import Path
from apply_portrait52 import make_edits as evf_edits,sha,PIN
from lcd_layout import plan
from layout_data import BASE,SOURCE
def make_edits(reference):
 assert sha(reference)==PIN
 edits=evf_edits(reference)
 for x in plan(reference):
  a=x['address'];old=reference[a-BASE:a-BASE+68];new=bytearray(old);struct.pack_into('<4h',new,32,*x['new'])
  edits.append(dict(x,address=hex(a),expected=old.hex(),replacement=bytes(new).hex()))
 assert len({e['address'] for e in edits})==len(edits)
 return sorted(edits,key=lambda x:int(x['address'],16))
def apply(image,reference):
 edits=make_edits(reference);d=bytearray(image)
 for e in edits:
  o=int(e['address'],16)-BASE;old=bytes.fromhex(e['expected']);assert d[o:o+68]==old,('record differs',e['address']);d[o:o+68]=bytes.fromhex(e['replacement'])
 rev=bytearray(d)
 for e in edits:
  o=int(e['address'],16)-BASE;rev[o:o+68]=bytes.fromhex(e['expected'])
 assert bytes(rev)==image
 return bytes(d),edits
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('input');p.add_argument('output');p.add_argument('--reference',default=str(SOURCE));a=p.parse_args()
 original=Path(a.input).read_bytes();d,edits=apply(original,Path(a.reference).read_bytes());Path(a.output).write_bytes(d)
 report={'input_sha256':sha(original),'sha256':sha(d),'changed_bytes':sum(x!=y for x,y in zip(original,d)),'records':len(edits),'edits':edits,'scope':'LCD REC+elapsed row2; long TC row5; EVF normal/small portrait. All landscape preserved. Bootguard caller-owned.'}
 Path(a.output+'.json').write_text(json.dumps(report,indent=2)+'\n');print({k:v for k,v in report.items() if k!='edits'})
