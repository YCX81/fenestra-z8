#!/usr/bin/env python3
"""Synthetic sequences and real encoded video/PTS, plus production C waveform."""
import bisect,csv,random,tempfile,unittest,subprocess,sys
from fractions import Fraction
from pathlib import Path
import video_decoder as dec

def bits_for(stage,value,damage=False):
    # Independent polynomial long division (not the decoder's shift register).
    code = (value if stage==1 else 0) if stage<2 else value if stage==2 and value<=5 else 6 if stage==2 else {0:0,1:1,255:2}.get(value,6)
    ext = (32 if stage==2 else 8) if code==6 else 0
    header = (stage<<3)|code
    payload = (header<<ext)|value if ext else header
    n=5+ext
    if stage==4:payload=(4<<7)|value;n=10
    remainder=payload<<4
    for i in range(n+3,3,-1):
        if remainder>>i&1:remainder^=0x13<<(i-4)
    full=(payload<<4)|remainder
    if damage:full^=1
    return [(full>>i)&1 for i in range(n+3,-1,-1)]

def frame_wave(stage,value,start=0,damage=False):
    t=start;wave=[]
    def pulse(level,duration):
        nonlocal t
        wave.append((t,level));t+=duration
    h=.025 if stage==4 else .050
    pulse(0,h*4);pulse(1,h*5);pulse(0,h*3)
    for bit in bits_for(stage,value,damage):
        pulse(1-bit,h);pulse(bit,h)
    wave.append((t,0))
    return wave,t

def sample(wave,end,fps=60,seed=2,jitter=.0025):
    rng=random.Random(seed);ts=[t for t,_ in wave];out=[]
    for i in range(int(end*fps)+1):
        t=i/fps+(rng.uniform(-jitter,jitter) if i else 0)
        k=bisect.bisect_right(ts,t)-1
        level=wave[k][1] if k>=0 else 0
        out.append((t,20+200*level+rng.uniform(-4,4)))
    return out

BUILD=Path(__file__).resolve().parents[2]/'recorder-build'

def accepted(rows):return [(r['stage'],r['value']) for r in rows if r['accepted']]

class Tests(unittest.TestCase):
    def test_short_stages(self):
        for phase in (1,2,10,11,13,15,17,18):
            for mask in range(4):
                value=(phase<<2)|mask
                bits=bits_for(4,value)
                row=dec.parse_bits(bits,short=True)
                self.assertTrue(row['accepted']);self.assertEqual(row['close_stage'],phase)
                self.assertEqual(row['nr_mask'],mask)
                for k in range(len(bits)):
                    bad=bits.copy();bad[k]^=1
                    try:r=dec.parse_bits(bad,short=True)
                    except ValueError:continue
                    self.assertFalse(r['accepted'])
                wave,end=frame_wave(4,value,.3)
                for seed in range(10):
                    rows,_,_=dec.decode_samples(sample(wave,end+.3,fps=120,jitter=.001,seed=seed))
                    self.assertEqual(accepted(rows),[('D',value)])

    def test_golden(self):
        self.assertEqual(''.join(map(str,bits_for(0,0))),'000000000')
        # Full data/remainder produces zero under polynomial division.
        for stage,values in [(0,[0]),(1,[0,1,2,3]),(2,[0,1,2,3,4,5,6,0xdeadbeef,0xffffffff]),(3,[0,1,255,2,42,254])]:
            for value in values:
                row=dec.parse_bits(bits_for(stage,value))
                self.assertTrue(row['accepted']);self.assertEqual(row['value'],value)

    def test_60fps_jitter_and_ordinary_light(self):
        wave=[(0,0),(.3,1),(1.3,0)];end=1.7;expect=[]
        for stage,value in [(0,0),(1,0),(1,1),(1,2),(1,3),(2,0),(2,5),(2,0xdeadbeef),(3,0),(3,1),(3,255),(3,42)]:
            w,end=frame_wave(stage,value,end);wave+=w;end+=.3;expect.append((dec.STAGES[stage],value))
        for seed in range(30):
            rows,_,_=dec.decode_samples(sample(wave,end,seed=seed))
            self.assertEqual(accepted(rows),expect,seed)
            self.assertTrue(any('sync high' in (r.get('error') or '') for r in rows))
            self.assertTrue(all(r['end_s']>r['sync_rise_s'] for r in rows if r['accepted']))

    def test_every_single_bit_error(self):
        for stage,value in [(0,0),(1,0),(2,3),(2,0xdeadbeef),(3,255),(3,42)]:
            original=bits_for(stage,value)
            for k in range(len(original)):
                damaged=original.copy();damaged[k]^=1
                try:row=dec.parse_bits(damaged)
                except ValueError:continue
                self.assertFalse(row['accepted'],(stage,value,k))

    def test_120fps(self):
        wave,end=frame_wave(2,0xffffffff,.3)
        rows,_,_=dec.decode_samples(sample(wave,end+.3,fps=120,jitter=.001))
        self.assertEqual(accepted(rows),[('S',0xffffffff)])

    def test_crc_rejection_and_recovery(self):
        wave,end=frame_wave(2,5,.3,damage=True)
        w,end=frame_wave(3,255,end+.3);wave+=w
        rows,_,_=dec.decode_samples(sample(wave,end+.3))
        self.assertEqual(accepted(rows),[('M',255)])
        self.assertTrue(any(r.get('crc_ok') is False and r['error']=='CRC mismatch' for r in rows))

    def test_interference_truncation_and_gap(self):
        wave,end=frame_wave(2,4,.3)
        # Force high across payload: no positive frame, even with a good sync.
        wave=[p for p in wave if p[0]<1.0]+[(1.0,1),(2.0,0)]
        rows,_,_=dec.decode_samples(sample(wave,2.4));self.assertEqual(accepted(rows),[])
        wave,end=frame_wave(2,0xdeadbeef,.3)
        rows,_,_=dec.decode_samples(sample(wave,2));self.assertEqual(accepted(rows),[])
        seq=sample(wave,end+.3);seq=[p for p in seq if not 1.0<p[0]<1.2]
        rows,_,_=dec.decode_samples(seq);self.assertEqual(accepted(rows),[])
        with self.assertRaises(ValueError):dec.decode_samples([(0,0),(0,100)])
        self.assertEqual(dec.decode_samples([(0,30),(.02,30)])[0],[])

    def test_video_pts_roi_and_outputs(self):
        import av,numpy as np
        wave=[(0,0),(.3,1),(1.3,0)]
        w,end=frame_wave(2,0xdeadbeef,1.7);wave+=w
        w,end=frame_wave(3,42,end+.3);wave+=w
        w,end=frame_wave(1,0,end+.3,damage=True);wave+=w
        seq=[(t+7.25,v) for t,v in sample(wave,end+.3)]
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'phone.mkv'
            with av.open(str(path),'w') as container:
                stream=container.add_stream('ffv1',rate=60);stream.width=80;stream.height=48;stream.pix_fmt='gray'
                stream.time_base=Fraction(1,1000000);stream.codec_context.time_base=Fraction(1,1000000)
                for t,brightness in seq:
                    pixels=np.full((48,80),30,dtype=np.uint8);pixels[8:20,12:28]=round(brightness)
                    frame=av.VideoFrame.from_ndarray(pixels,format='gray');frame.pts=round(t*1000000);frame.time_base=Fraction(1,1000000)
                    for packet in stream.encode(frame):container.mux(packet)
                for packet in stream.encode():container.mux(packet)
            actual=dec.video_samples(path,(12,8,16,12))
            rows,levels,meta=dec.decode_samples(actual)
            self.assertEqual(accepted(rows),[('S',0xdeadbeef),('M',42)])
            self.assertTrue(any(r.get('crc_ok') is False for r in rows))
            self.assertEqual(len(actual),len(seq))
            self.assertLess(max(abs(a[0]-b[0]) for a,b in zip(actual,seq)),.0011)
            self.assertGreater(actual[0][0],7)
            self.assertGreater(len({round(b[0]-a[0],3) for a,b in zip(actual,actual[1:])}),2)
            paths=dec.write_outputs(Path(tmp)/'result',actual,rows,levels,meta)
            self.assertTrue(all(p.exists() for p in paths))
            with self.assertRaises(ValueError):dec.video_samples(path,(70,0,20,10))
            run=subprocess.run([sys.executable,str(Path(dec.__file__)),str(path),'--roi','12','8','16','12'],capture_output=True,text=True)
            self.assertEqual(run.returncode,0,run.stderr)
            self.assertTrue(path.with_suffix('.lamp.frames.json').exists())

    def test_production_c_wave(self):
        path=BUILD/'recorder-check/lamp-c-wave.csv'
        self.assertTrue(path.exists(),'run test_io.py recorder-build first')
        with path.open() as f:wave=[(int(r['time_us'])/1e6,int(r['level'])) for r in csv.DictReader(f)]
        rows,_,_=dec.decode_samples(sample(wave,wave[-1][0],fps=120,jitter=.001))
        self.assertEqual(accepted(rows),[('I',0),('R',0),('R',1),('R',2),('R',3),('S',0),('S',5),('S',0xdeadbeef),('M',0),('M',1),('M',255),('M',42),('R',3),('D',71)])

if __name__=='__main__':
    import argparse
    p=argparse.ArgumentParser(add_help=False);p.add_argument('--build',type=Path,default=BUILD)
    a,rest=p.parse_known_args();BUILD=a.build
    unittest.main(argv=[sys.argv[0],*rest],verbosity=2)
