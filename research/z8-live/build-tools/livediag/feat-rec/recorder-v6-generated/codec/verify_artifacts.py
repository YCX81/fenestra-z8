import hashlib,json,pathlib,subprocess,sys
from PIL import Image
source=pathlib.Path(sys.argv[1]); out=pathlib.Path(sys.argv[2]); pair=out/'paired.JPG'
a,b=source.read_bytes(),pair.read_bytes()
assert len(a)==len(b)
# Changed ranges must lie before scan data; first image's entropy bytes and MPO tail unchanged.
sos=a.find(b'\xff\xda'); assert sos>0 and a[sos:]==b[sos:]
images=[]
with Image.open(source) as x,Image.open(pair) as y:
 assert x.n_frames==y.n_frames
 for i in range(x.n_frames):
  x.seek(i);y.seek(i);x.load();y.load()
  assert x.size==y.size and x.tobytes()==y.tobytes()
  images.append({'frame':i,'size':x.size,'pixels_sha256':hashlib.sha256(x.tobytes()).hexdigest()})
p=json.loads(pathlib.Path('codec/ffprobe.json').read_text())
v=p['streams'][0];m=p['streams'][1]
assert v['codec_name']=='h264' and v['profile']=='Baseline' and v['level']==30 and (v['width'],v['height'])==(640,424)
assert v['nb_frames']=='90' and m['codec_tag_string']=='mebx'
vp=[i for i in p['packets'] if i['stream_index']==0];mp=[i for i in p['packets'] if i['stream_index']==1]
assert len(vp)==90 and len(mp)==1 and mp[0]['pts']==1499985
assert sum(i['duration'] for i in vp)==3033303 and sum(i['duration']==66666 for i in vp)==1
mov=(out/'test.mov').read_bytes();pos=int(mp[0]['pos']);assert mov[pos:pos+9]==bytes.fromhex('0000000900000001ff')
assert b'com.apple.quicktime.still-image-time' in mov
assert b'dtyp\0\0\0\0\0\0\0A' in mov
uuid='4C495645-5048-4F54-8F41-303030303031'
for f in [pair,out/'test.mov']:
 assert subprocess.check_output(['exiftool','-s3','-ContentIdentifier',str(f)],text=True).strip()==uuid
print(json.dumps({'native_source':str(source),'native_bytes':len(a),'paired_bytes':len(b),'all_mpo_pixels_identical':images,'mov_bytes':len(mov),'video_frames':90,'duration_us':3033303,'still_image_time_pts_us':mp[0]['pts'],'sample_hex':mov[pos:pos+9].hex(),'uuid':uuid},indent=2))
