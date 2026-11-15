"""Strict v52 portrait overlay. No code cave, landscape, resource or font changes.
Input may contain independently linked Live changes; every affected 68-byte native
record must still equal the pinned v51d record. Bootguard stamping is caller-owned.
"""
import argparse,hashlib,json,struct
from pathlib import Path
from evf_layout import plan
from layout_data import BASE,SOURCE
PIN='aa6e94e0042fb445c666de73c0f6b9e7e569010b7993a3ac3da0979b174a9433'
def sha(b):return hashlib.sha256(b).hexdigest()
def make_edits(reference):
 assert sha(reference)==PIN
 edits={}
 for x in plan(reference):
  a=x['address'];old=reference[a-BASE:a-BASE+68];new=bytearray(old)
  struct.pack_into('<4h',new,32,*x['new'])
  for s,d in ((8,12),(16,20),(40,44),(48,52)):new[d:d+4]=old[s:s+4]
  item=dict(x,address=hex(a),expected=old.hex(),replacement=bytes(new).hex())
  if a in edits:assert edits[a]['replacement']==item['replacement']
  else:edits[a]=item
 return sorted(edits.values(),key=lambda x:int(x['address'],16))
def apply(image,reference):
 edits=make_edits(reference);d=bytearray(image)
 for e in edits:
  o=int(e['address'],16)-BASE;old=bytes.fromhex(e['expected']);new=bytes.fromhex(e['replacement'])
  assert d[o:o+len(old)]==old,('record differs',e['address'])
  d[o:o+len(old)]=new
 rev=bytearray(d)
 for e in edits:
  o=int(e['address'],16)-BASE;rev[o:o+68]=bytes.fromhex(e['expected'])
 assert bytes(rev)==image
 return bytes(d),edits
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('input');p.add_argument('output');p.add_argument('--reference',default=str(SOURCE));a=p.parse_args()
 original=Path(a.input).read_bytes();d,edits=apply(original,Path(a.reference).read_bytes());Path(a.output).write_bytes(d)
 report={'input_sha256':sha(original),'sha256':sha(d),'changed_bytes':sum(x!=y for x,y in zip(original,d)),'records':len(edits),'edits':edits,'scope':'EVF normal/small video portrait fields; landscape preserved; Bootguard stamp pending owner build'}
 Path(a.output+'.json').write_text(json.dumps(report,indent=2)+'\n');print({k:v for k,v in report.items() if k!='edits'})
