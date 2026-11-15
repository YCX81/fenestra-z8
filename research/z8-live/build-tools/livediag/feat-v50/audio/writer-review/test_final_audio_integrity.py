"""Validate final actual C files across track, NCTG, LVJB and source PCM."""
from pathlib import Path
import json
import struct
import sys

HERE=Path(__file__).resolve().parent
WRITER=HERE.parents[1]/'writer'
sys.path.insert(0,str(HERE.parents[2]/'feat-mov'))
import mov_fix as m

pcm=(WRITER/'input.s16').read_bytes()
expected=b''.join(b'\0'+pcm[i:i+2] for i in range(0,len(pcm),2))
results=[]
for name,has_audio in [('audio',True),('silent',False),('audio-read-fallback',False)]:
    path=HERE/'audio-paths'/(name+'.MOV');data=path.read_bytes();roots=m.parse(data)
    moov=m.direct_child(roots,b'moov')
    tracks=[t for t in moov.children if t.typ==b'trak']
    audio=[t for t in tracks if t.one(b'mdia').one(b'hdlr').data[8:12]==b'soun']
    assert bool(audio)==has_audio
    ncdt=m.direct_child(m.parse(moov.one(b'udta').data),b'NCDT')
    tg=m.direct_child(m.parse(ncdt.data),b'NCTG').data
    pos=0;fields={}
    while pos<len(tg):
        tag,kind,count=struct.unpack_from('>IHH',tg,pos);n=count*(0,1,1,2,4,8,1,1,2,4,8)[kind]
        fields[tag]=tg[pos+8:pos+8+n];pos+=8+n
    assert len(fields)==19 and pos==len(tg)
    assert [int.from_bytes(fields[t],'big') for t in range(0x31,0x35)]==([1,2,24,48000] if has_audio else [0]*4)
    # Locate LVJB from the indexed JPEG end, independently of free-atom padding.
    n=struct.unpack_from('<I',data,len(data)-8)[0];free_at=len(data)-n
    assert data[free_at+4:free_at+8]==b'free'
    count=struct.unpack_from('<I',data,free_at+20)[0]
    head=struct.unpack_from('<H',data,free_at+14)[0]
    sizes=struct.unpack_from('<'+'I'*count,data,free_at+32)
    record=free_at+8+head+sum(sizes)
    if has_audio:
        assert data[record:record+4]==b'LVJB'
        off,frames,rate=struct.unpack_from('<III',data,record+4)
        assert rate==48000 and frames==len(pcm)//4
        assert data[off:off+frames*6]==expected
        st=audio[0].one(b'mdia').one(b'minf').one(b'stbl')
        desc=st.one(b'stsd').data[8:]
        assert desc[4:8]==b'lpcm' and int.from_bytes(desc[16:18],'big')==2
        assert struct.unpack_from('>d',desc,40)[0]==48000
        assert struct.unpack_from('>IIII',desc,48)==(2,0x7f000000,24,12)
        assert struct.unpack_from('>II',desc,64)==(6,1)
        sample_bytes,sample_count=struct.unpack_from('>II',st.one(b'stsz').data,4)
        assert (sample_bytes,sample_count)==(6,frames)
        offsets=list(struct.unpack_from('>'+str(m.u32(st.one(b'stco').data,4))+'I',st.one(b'stco').data,8))
        assert offsets==list(range(off,off+frames*6,24024*6))
    else:
        assert data[record:record+4] not in (b'LVJA',b'LVJB')
    results.append({'name':name,'nctg_audio':has_audio,'lpcm_v2_le24':has_audio,
                    'source_pcm_bit_exact':has_audio,'lvjb':has_audio,
                    'apple_marker':b'com.apple.quicktime.still-image-time' in data})
assert all(x['apple_marker'] for x in results)
(HERE/'audio-paths/audio-integrity.json').write_text(json.dumps({'status':'PASS','results':results},indent=2)+'\n')
print('PASS source s16 -> signed LE24 / track tables / NCTG / LVJB; silent and partial fallback contain no audio references')
