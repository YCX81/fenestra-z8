"""Strict portrait-only delta over the delivered v52 G geometry.

The three edge labels are native groups 34 (resolution/FPS), 37 (image area),
73 (codec), proved by updater -> resource lookup -> PNG inspection. Rectangles
keep their original sizes and styles. Renderer centers EVF foreground assets
inside their larger native boxes; LCD also has the larger background layer.
"""
import argparse, hashlib, json, struct
from pathlib import Path
from layout_data import BASE, SOURCE, rows, mapping

PIN='ed1e8496fccde443412e527a252de5029a55c749053b9b046da42bfc454aa613'
def sha(b): return hashlib.sha256(b).hexdigest()
def shift(r,dx,dy): return tuple(v+(dx if j%2==0 else dy) for j,v in enumerate(r))

def edits(reference):
    assert sha(reference)==PIN, 'reference must be delivered v52 G'
    out={}
    maps=mapping(reference)
    for table in range(4):
        data=rows(reference,table)
        for m in maps:
            g,j=m['group'],m['index']; ident=m['video'][table]
            if ident not in data: continue
            r=data[ident]; old=r['P']; new=old; why=''
            if not any(old): continue
            if table<2:
                if g==34:
                    new=shift(old,36,0); why='resolution aligns LCD background plate right=476'
            else:
                small=table==3; sy=74 if small else 0
                right=828 if small else 894
                if g==34:
                    new=(right-68,60+sy,right,96+sy)
                    why='resolution foreground right matches FX/codec foreground'
                elif g==37:
                    new=(right-40,112+sy,right,148+sy)
                    why='image area at row3 in common right column'
                elif g==73:
                    new=(right-86,164+sy,right,200+sy)
                    why='codec at row4 in common right column'
                elif g in (30,32):
                    source=(166,134) if small else (100,60)
                    new=shift(old,386-source[0],60+sy-source[1])
                    why='REC at row2 center, preserving every compound offset'
                elif g==66:
                    if j==2:
                        new=(470,66+sy,600,90+sy)
                        why='elapsed time beside REC; 8px gap'
                    else:
                        width=old[2]-old[0]
                        new=(391,222+sy,391+width,246+sy)
                        why='long timecode keeps separate row5 like LCD'
                elif g in (33,38):
                    dx,dy=(-124,964) if small else(-130,1104)
                    new=shift(old,dx,dy)
                    why='remaining time and card label return to bottom status area'
                elif small and (g==31 or(g==35 and j<34)):
                    new=shift(old,-12,0)
                    why='small EVF audio band clearance from bottom card/remaining time'
            if new==old: continue
            width,height=(480,720) if table<2 else(960,1280)
            assert 0<=new[0]<new[2]<=width and 0<=new[1]<new[3]<=height
            assert (new[2]-new[0],new[3]-new[1])==(old[2]-old[0],old[3]-old[1])
            addr=r['address']; record=reference[addr-BASE:addr-BASE+68]
            replacement=bytearray(record);struct.pack_into('<4h',replacement,32,*new)
            item=dict(table=table,group=g,index=j,id=ident,address=hex(addr),
                old=old,new=new,reason=why,expected=record.hex(),replacement=bytes(replacement).hex())
            if addr in out: assert out[addr]['replacement']==item['replacement']
            else: out[addr]=item
    return [out[a] for a in sorted(out)]

def apply(image,reference):
    records=edits(reference); out=bytearray(image)
    for e in records:
        off=int(e['address'],16)-BASE; expect=bytes.fromhex(e['expected'])
        assert out[off:off+68]==expect, ('wrong v52 geometry',e['address'])
        out[off:off+68]=bytes.fromhex(e['replacement'])
    undo=bytearray(out)
    for e in records:
        off=int(e['address'],16)-BASE;undo[off:off+68]=bytes.fromhex(e['expected'])
    assert bytes(undo)==image
    return bytes(out),records

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('input');p.add_argument('output');p.add_argument('--reference',default=str(SOURCE));a=p.parse_args()
    old=Path(a.input).read_bytes();new,records=apply(old,Path(a.reference).read_bytes())
    Path(a.output).write_bytes(new)
    report=dict(input_sha256=sha(old),sha256=sha(new),reference_sha256=PIN,records=len(records),
        changed_bytes=sum(a!=b for a,b in zip(old,new)),edits=records,
        scope='Portrait rectangles only; native bitmap/font/styles and all landscape unchanged; caller recomputes bootguard/buildID')
    Path(a.output+'.json').write_text(json.dumps(report,indent=2)+'\n')
    print({k:v for k,v in report.items() if k!='edits'})
