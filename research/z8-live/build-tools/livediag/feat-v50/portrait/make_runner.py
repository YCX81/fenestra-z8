#!/usr/bin/env python3
"""Derive an isolated video orientation smoke lane from the existing lab runner."""
from pathlib import Path

HERE = Path(__file__).resolve().parent
s = (HERE/'run_movie_reference.py').read_text()
s = s.replace('HERE=Path(__file__).resolve().parent',
    "HERE=Path.home()/'.local/share/nikon-z8-qemu/live-audio/regress/rerun-w10/retry-driver'")
s = s.replace("RUNROOT=BASE/'live-audio/regress/runs'", "RUNROOT=Path.home()/'fxfix/v50-portrait/runs'")
s = s.replace("OFFSET_PATH=HERE/'v38-offsets.json'", "OFFSET_PATH=Path(os.environ['PORTRAIT_OFFSETS'])")
s = s.replace("    if label=='movie-lever':MOVIE[0]=True", """    if label.startswith('pose-'):
        pose=label.split('-',1)[1]
        sample={'left':'0000c0000000','right':'000040000000','landscape':'c00000000000'}[pose]
        self.g.qmp.command('qom-set',path='/machine',property='orientation-input',value=sample)
        actual=self.g.qmp.command('qom-get',path='/machine',property='orientation-input')
        if actual!=sample:raise RuntimeError('orientation sensor input readback mismatch')
        self.g.data.setdefault('portrait_sensor_inputs',[]).append(dict(pose=pose,sample=sample))
    if label=='movie-lever':MOVIE[0]=True""")
s = s.replace("    s={'label':label,", """    s={'native_orientation':rd(0xa03228244,4),'orientation_classification':rd(0xa02e560c8+0xc4,4),
       'effective_screen_suppression':rd(0xa03228308,4),'auto_rotate_setting':rd(0xa03d70940,4),'evf_rotate_setting':rd(0xa03d7093c,4),'label':label,""")
start=s.index('def contacts(args):')
end=s.index('probe.contact_sequence=contacts',start)
s=s[:start]+'''def contacts(args):
    global LIVE_CONTACTS
    rows=[]
    def contact(label,movie=False,gap=5,half=False):
        b=bytearray(_kb_raw(half=half));b[0x57]=int(movie)
        e=bytearray(probe.expected_keys(half=half))
        if movie:struct.pack_into('<I',e,0x24,struct.unpack_from('<I',e,0x24)[0]|0x10000)
        rows.append(dict(label=label,body=bytes(b),expected=bytes(e),gap=int(gap*1e9),half=half,full=False))
    contact('wake-half',half=True,gap=1)
    contact('wake-release',gap=12)
    contact('photo-landscape',gap=5)
    contact('pose-left',gap=8)
    contact('photo-left',gap=5)
    contact('movie-lever',movie=True,gap=12)
    contact('video-left',movie=True,gap=6)
    contact('pose-right',movie=True,gap=8)
    contact('video-right',movie=True,gap=6)
    contact('pose-landscape',movie=True,gap=8)
    contact('video-landscape',movie=True,gap=6)
    contact('photo-lever',gap=10)
    contact('post-complete',gap=5)
    LIVE_CONTACTS=rows
    return rows,len(rows)
''' + s[end:]
s = s.replace("g=super().__enter__();original=g.qmp.command", "g=super().__enter__();original=g.qmp.command\n        if os.environ.get('PORTRAIT_VIDEO_SERIAL'):\n            original('qom-set',path='/machine',property='z8-video-serial-input',value='0:0:0:0')")
# Models are sensor inputs; the full histogram lab/temporary bypass is absent.
s = s.replace("extra+=['z8-serial5-config-model=on'", "extra+=['orientation-fixture=on','i2c-fixture=on','z8-serial5-config-model=on'")
(HERE/'run_orientation.py').write_text(s)
print(HERE/'run_orientation.py')
