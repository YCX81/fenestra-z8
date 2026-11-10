#!/usr/bin/env python3
"""Playback layer: RAW+JPEG. The native current file can be DSC_xxxx.NEF while the Live manifest
names DSC_xxxx.JPG; match the 26-byte stem (A:\\DCIM\\nnnNCZ_8\\DSC_xxxx.) and check the size of
the manifest's own file. Usage: pb_patch.py PB_SRC_DIR"""
import sys
from pathlib import Path
src = Path(sys.argv[1])
p = src / 'runtime.c'; t = p.read_text()
old = '''    memcmp(r->manifest.native_path,r->native_path,30)||r->manifest.volume_generation!=LOAD(&r->media_epoch)||
    readfile(r->native_path,0,0,0,&size)||size!=r->manifest.native_size){r->lookup_error=2;return;}'''
new = '''    memcmp(r->manifest.native_path,r->native_path,26)||r->manifest.volume_generation!=LOAD(&r->media_epoch)||
    readfile(r->manifest.native_path,0,0,0,&size)||size!=r->manifest.native_size){r->lookup_error=2;return;}'''
assert t.count(old) == 1; p.write_text(t.replace(old, new)); print('pb patch applied to', src)
