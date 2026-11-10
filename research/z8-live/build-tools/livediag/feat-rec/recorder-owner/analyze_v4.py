"""Analyze exported run artifacts only; no guest control or media edits."""
import hashlib,json,subprocess,sys
from pathlib import Path
from PIL import Image
D=Path(__file__).resolve().parent
R=Path('/path/to/user/.local/share/nikon-z8-qemu/live-a-driver-20261104')
def run(args):return subprocess.check_output(args,text=True)
def pixels(a,b):
 rows=[]
 with Image.open(a) as x,Image.open(b) as y:
  assert x.n_frames==y.n_frames
  for i in range(x.n_frames):
   x.seek(i);y.seek(i);x.load();y.load();bx=x.tobytes();by=y.tobytes()
   assert x.size==y.size and bx==by
   rows.append(dict(frame=i,size=x.size,pixels_sha256=hashlib.sha256(bx).hexdigest()))
 return rows
for name in sys.argv[1:]:
 folder=R/name;out={'run':name,'media':[]}
 rows=[json.loads(x) for x in (folder/'live-checkpoints.jsonl').read_text().splitlines()]
 out['last_status']=rows[-1].get('runtime',{})
 active=[x for x in rows if x.get('runtime',{}).get('capture.actual_bytes')]
 if active:out['active_status']=active[-1]['runtime']
 native=sorted((folder/'files/DCIM').rglob('*.JPG')) if (folder/'files/DCIM').exists() else []
 out['native_jpegs']=[dict(path=str(p),bytes=p.stat().st_size,size=Image.open(p).size) for p in native]
 if name.endswith('off'):
  control=R/'retry-control-off/files/DCIM/100NCZ_8/DSC_0001.JPG'
  out['control']=str(control);out['pixels_equal_control']=pixels(control,native[0]);assert not (folder/'files/LIVE').exists()
 for mov in sorted((folder/'files').rglob('CLIP.MOV')):
  stem=name+'-'+mov.parent.name
  probe=run(['ffprobe','-v','error','-show_streams','-show_format','-show_packets','-of','json',str(mov)])
  (D/(stem+'-ffprobe.json')).write_text(probe);p=json.loads(probe);v=p['streams'][0]
  vp=[x for x in p['packets'] if x['stream_index']==0];mp=[x for x in p['packets'] if x['stream_index']==1]
  assert v['codec_name']=='h264' and v['profile']=='Baseline' and v['level']==31 and v['width']==1024 and 'K' in vp[0]['flags'] and len(mp)==1
  md5=D/(stem+'-framemd5.txt');subprocess.run(['ffmpeg','-v','error','-y','-i',str(mov),'-map','0:v:0','-f','framemd5',str(md5)],check=True)
  hashes=[x.split(',')[-1].strip() for x in md5.read_text().splitlines() if not x.startswith('#')]
  jpg=mov.parent/'PAIR.JPG';manifest=(mov.parent/'MANIFEST').read_text();src=next(x for x in native if x.stem==mov.parent.name)
  ids=[run(['exiftool','-s3','-ContentIdentifier',str(x)]).strip() for x in (jpg,mov)];assert ids[0]==ids[1] and ids[0] in manifest
  exif=run(['exiftool','-ContentIdentifier','-ImageWidth','-ImageHeight','-MPImageType',str(src),str(jpg),str(mov)])
  (D/(stem+'-exiftool.txt')).write_text(exif)
  data=mov.read_bytes();at=int(mp[0]['pos']);sample=data[at:at+9].hex();assert sample=='0000000900000001ff'
  status=out.get('active_status',out['last_status'])
  # For multiple pairs, use its completed press checkpoint (latest status belongs to last shot).
  matching=[x['runtime'] for x in rows if x.get('runtime',{}).get('debug_ticket_path','').endswith(src.name)]
  if matching:status=matching[-1]
  anchor=status.get('stream.clip_press',0)-status.get('stream.snapshot_first',0)
  if anchor>0:assert int(mp[0]['pts'])<=anchor<int(mp[0]['pts'])+int(p['streams'][1]['duration_ts'])
  media=dict(native=str(src),pair=str(jpg),mov=str(mov),manifest=manifest,bytes=mov.stat().st_size,width=v['width'],height=v['height'],frames=len(vp),duration=v['duration'],bitrate=v.get('bit_rate'),format_bitrate=p['format'].get('bit_rate'),unique_frame_hashes=len(set(hashes)),idr_pts=[x['pts'] for x in vp if 'K' in x['flags']],still_image_time_pts=mp[0]['pts'],still_image_time_duration=p['streams'][1]['duration_ts'],press_relative_us=anchor,sample_hex=sample,uuid=ids[0],pair_pixels_equal_native=pixels(src,jpg))
  assert len(set(hashes))>1
  out['media'].append(media)
 (D/(name+'-analysis.json')).write_text(json.dumps(out,indent=2)+'\n')
 print(name,{'native':len(native),'clips':len(out['media']),'results':[{k:m[k] for k in ('width','height','frames','duration','bytes','unique_frame_hashes','uuid')} for m in out['media']]})
