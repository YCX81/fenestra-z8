#!/usr/bin/env python3
"""Adapt one isolated G capture harness to V62, exact identity and native menus."""
import argparse, hashlib, json, re, subprocess
from pathlib import Path
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--driver',type=Path,required=True)
ap.add_argument('--main',type=Path,required=True)
ap.add_argument('--qemu',type=Path,required=True)
ap.add_argument('--merged',action='store_true',help='Use the complete capture profile based on QEMU 10.2.4')
ap.add_argument('--short-idle',action='store_true',help='Observe 27 seconds after the photo, ending before native standby')
a=ap.parse_args();p=a.driver/'retry-driver/run_live.py';s=p.read_text()
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
def sub(old,new):
 global s
 assert s.count(old)==1,(old,s.count(old))
 s=s.replace(old,new)
sub("P.add_argument('--mode',", "P.add_argument('--sound', choices=['on','off'], required=True)\nP.add_argument('--hook-manifest',type=Path,required=True)\nP.add_argument('--mode',")
sub("manifest=json.loads((A.main.parent/'changed-ranges.json').read_text())", "manifest=json.loads(A.hook_manifest.read_text())")
sub("'photo_title':(0x432a7fac,'PHOTO SHOOTING MENU')", "'photo_title':(0x432a7fac,'PHOTO SHOOTING MENU'),\n            'camera_sounds':(0x432b625c,'Camera sounds'),\n            'shutter_sound':(0x432b8a90,'Shutter sound'),\n            'sound_type':(0x432b8aae,'Type'),\n            'type_b':(0x432b8ac2,'Type B')")
sub('LIVE_SELECTED=False', 'LIVE_SELECTED=False\nPROFILE_SELECTED=False\nSOUND_SELECTED=False')
sub('global TAB_ENTERED,QUALITY_SELECTED,LIVE_SELECTED','global TAB_ENTERED,QUALITY_SELECTED,LIVE_SELECTED,PROFILE_SELECTED,SOUND_SELECTED')
sub("    elif kind.startswith('live-seek-'):", """    elif kind.startswith('profile-seek-'):
        if not PROFILE_SELECTED:
            ids=native_text_ids(g)
            wanted=[ids.get(x) for x in ['camera_sounds','shutter_sound','sound_type','type_b']]
            # Camera sounds includes a non-actionable Shutter sound section
            # heading. Choose the deepest available real row (Type/B).
            matches=[r for text_id in reversed(wanted) for r in menu.get('rows',[]) if r['text_id']==text_id]
            if matches:
                chosen=matches[0];key=direction(menu,chosen['index'])
                if key=='ok' and chosen['text_id']==ids.get('type_b'):PROFILE_SELECTED=True
    elif kind.startswith('sound-seek-'):
        desired=int(A.sound=='off')
        if info.get('native_audio',{}).get('live_mute')==desired:SOUND_SELECTED=True
        elif not SOUND_SELECTED and page in (0x7000,0x705e):
            target=0x7fb2 if page==0x7000 else 0x7fb3
            chosen=next((r['index'] for r in menu.get('rows',[]) if r['row_id']==target),None)
            if chosen is not None:key=direction(menu,chosen)
    elif kind.startswith('live-seek-'):""")
s=re.sub(r"EXPECTED_MAIN='[a-f0-9]{64}'", "EXPECTED_MAIN='"+sha(a.main)+"'",s,count=1)
s=re.sub(r"assert sha\(A.qemu\)=='[a-f0-9]{64}'", "assert sha(A.qemu)=='"+sha(a.qemu)+"'",s,count=1)
sub("PB_WIDTHS=json.loads((TASK/'pb-widths.json').read_text())", "PB_WIDTHS=json.loads((TASK/'pb-widths.json').read_text())\nUI_OFFSETS=json.loads((TASK/'ui-offsets.json').read_text())\nUI_WIDTHS=json.loads((TASK/'ui-widths.json').read_text())")
sub("at=PB_OFFSETS[field];s['pb'][field]=int.from_bytes(pb_blob[at:at+width],'little')", "at=PB_OFFSETS[field];data=pb_blob[at:at+width]\n                    s['pb'][field]=(data.split(b'\\0')[0].decode('latin1') if field in ['native_path','directory','nb_from','nb_to'] else int.from_bytes(data,'little'))")
old="""    s.update(ui_magic=hex(rd(u,4)),save_error=rd(u+28,4),loaded=rd(u+32,4),generation=rd(u+48,8),
             indicator_attached=rd(u+12296,4),indicator_frames=rd(u+12300,4),indicator_error=rd(u+12312,4))"""
sub(old,"    s['ui_fields']={field:rd(u+UI_OFFSETS[field],width) for field,width in UI_WIDTHS.items()}\n    s.update(save_error=s['ui_fields']['save_error'],loaded=s['ui_fields']['loaded'],generation=s['ui_fields']['generation'])")
sub("'live_mute':rd(u+0x9434,4)","'live_mute':rd(u+UI_OFFSETS['mute_live'],4)")
sub("    shutter('wake-half',True);shutter('wake-release')", """    shutter('wake-half',True);shutter('wake-release')
    # Original MCU MODE/Main chord. Original decoder 41020844 independently
    # confirms MODE body0e -> SW0 bit11 and signed body0b -> word4.
    # A -> M is then verified by the actual shooting screenshot, not assumed
    # from delivery alone. No firmware setting/RAM injection is used.
    for label,held,delta in [('exposure-mode-hold',True,0),('exposure-mode-minus',True,-1),
                              ('exposure-mode-neutral',True,0),('exposure-mode-release',False,0)]:
        body=bytearray(probe.key_body());body[0x0e]=int(held);body[0x0b]=delta&255
        expected=struct.pack('<11I',0x200|(int(held)<<11),0,0,0,delta&0xffffffff,*([0]*6))
        rows.append(dict(label=label,body=bytes(body),expected=expected,gap=500000000 if held else 1000000000,half=False,full=False))""")
sub("            if chosen is not None:\n                key=direction(menu,chosen)\n                if page==0x705e and key=='ok':LIVE_SELECTED=True", """            if chosen is not None:
                key=direction(menu,chosen)
                if page==0x705e and key=='ok':LIVE_SELECTED=True
            elif page==1495:key='menu'
            elif page==1:key='right'""")
sub("        for i in range(20):key(f'live-seek-{i:02}','up')", """        for i in range(40):key(f'profile-seek-{i:02}','down')
        # Native Type selection returns to its parent. Re-enter the setup tab.
        shutter('profile-exit-half',True);shutter('profile-exit-release')
        key('profile-back-menu','menu',3);key('profile-back-left','left')
        # The tab remains Setup after this native half-press/reopen.
        key('profile-back-right','right')
        for i in range(40):key(f'live-seek-{i:02}','up')
        for i in range(6):key(f'sound-seek-{i:02}','down')""")
sub("                if path.stem.endswith('-photo-live-10s') and not QUALITY_SELECTED:", """                if path.stem.endswith('-photo-live-10s'):
                    assert PROFILE_SELECTED, 'Native Type B menu selection not reached'
                    assert SOUND_SELECTED and info.get('native_audio',{}).get('live_mute')==int(A.sound=='off'), 'Actual Live sound toggle not reached'
                if path.stem.endswith('-photo-live-10s') and not QUALITY_SELECTED:""")
# Export source consumption at every postcapture checkpoint, so a later failure
# does not erase direct sound evidence. This is read-only QOM state.
sub("                if path.stem=='final':\n                    # Final only,", "                if path.stem=='final' or '-post-photo-' in path.stem or '-post-complete' in path.stem:\n                    # Read-only consumption export,")
if a.short_idle:
 sub("shutter('post-encode-30s',gap=30)","shutter('post-encode-10s',gap=10)")
 sub("shutter('post-complete',gap=15)","shutter('post-complete',gap=5)")
p.write_text(s)
# Pin the exact complete V62 image as the declared experiment baseline. Keep
# stock length/identity and capture-ROM dependency byte checks intact.
resource=a.driver/'retry-driver/z8_capture_resources.py'
rs=resource.read_text();oldbase="base_sha='c770f34014849b2c42f20b9258a32b8959253cfb296e48f83417572781e00864'"
assert rs.count(oldbase)==1
resource.write_text(rs.replace(oldbase,"base_sha='"+sha(a.main)+"'"))
# Rebuild only passive observers. Drop obsolete injected-code PC addresses;
# keep pinned stock audio service/driver dispatch observations.
rx=a.driver/'rx_observer.c';r=rx.read_text()
start=r.index('static const char*diagnostic_site(');end=r.index('\nstatic uint64_t diagnostic_hits',start)
r=r[:start]+'static const char*diagnostic_site(uint64_t pc){(void)pc;return 0;}'+r[end:]
offsets=json.loads((a.driver/'runtime-offsets.json').read_text());widths=json.loads((a.driver/'runtime-widths.json').read_text())
for line in r.splitlines():
 if 'memcpy(&v,m[cpu]->data+' in line and 'fprintf(fp' in line:
  match=re.search(r'\\"([^"\\]+)\\":%llu',line)
  if match and match[1] in offsets:
   new=re.sub(r'm\[cpu\]->data\+\d+,\d+',f"m[cpu]->data+{offsets[match[1]]},{widths[match[1]]}",line)
   r=r.replace(line,new)
# Old PB model snapshots have no bearing on capture. Explicitly remove them;
# Python checkpoints read all current PB fields using freshly compiled ABI.
start=r.index(' uint64_t r=runtime(cpu);');end=r.index('\n}\nstatic const char*diagnostic_site',start)
r=r[:start]+r[end:]
rx.write_text(r)
flags=subprocess.check_output(['pkg-config','--cflags','--libs','glib-2.0'],text=True).split()
inc=('/path/to/user/.local/share/nikon-z8-qemu/merged/work/qemu-10.2.4/include/qemu' if a.merged else '/path/to/user/.local/share/nikon-z8-qemu/rdw/qemu-11.1.1/include/plugins')
# Older QEMU keeps this public header directly in include/qemu.
if a.merged and not (Path(inc)/'qemu-plugin.h').exists():
 inc='/path/to/user/.local/share/nikon-z8-qemu/merged/work/qemu-10.2.4/include'
for name in ['rx_observer','lv_observer']:
 src=a.driver/(name+'.c');c=src.read_text()
 # QEMU 11.1.1 public callback API adds explicit userdata.
 if not a.merged:
  c=re.sub(r'(static void \w+)\(qemu_plugin_id_t id,unsigned cpu\)',r'\1(unsigned cpu,void *id)',c)
  c=re.sub(r'(static void \w+)\(qemu_plugin_id_t id,struct qemu_plugin_tb\s*\*tb\)',r'\1(struct qemu_plugin_tb *tb,void *id)',c)
  c=re.sub(r'(qemu_plugin_register_vcpu_(?:init|tb_trans)_cb\(id,\w+)\)',r'\1,NULL)',c)
 src.write_text(c)
 subprocess.run(['gcc','-O2','-shared','-fPIC','-I'+inc,str(a.driver/(name+'.c')),'-o',str(a.driver/(name+'.so'))]+flags,check=True)
(a.driver/'v62-driver-identity.json').write_text(json.dumps({'main_sha256':sha(a.main),'qemu_sha256':sha(a.qemu),'driver_sha256':sha(p),'rx_observer_sha256':sha(rx),'scope':'Native menu/MCU inputs, functional RX/TX. No guest RAM writes or physical camera.'},indent=2)+'\n')
print('ADAPTED',p)
