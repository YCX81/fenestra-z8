"""Extract/assemble the test card's exFAT partition using regular files only."""
import os,sys,errno
from pathlib import Path
OFF=1048576

def copy_extents(src,dst,start,end,shift):
 p=start
 while p<end:
  try:p=os.lseek(src,p,os.SEEK_DATA)
  except OSError as e:
   if e.errno==errno.ENXIO:break
   raise
  if p>=end:break
  q=min(os.lseek(src,p,os.SEEK_HOLE),end)
  while p<q:
   n=min(q-p,1048576);data=os.pread(src,n,p);assert len(data)==n
   assert os.pwrite(dst,data,p+shift)==n;p+=n

mode,source,dest=sys.argv[1:4]
src=os.open(source,os.O_RDONLY);out=os.open(dest,os.O_RDWR|os.O_CREAT|os.O_EXCL,0o600)
try:
 size=os.fstat(src).st_size
 if mode=='extract':
  os.ftruncate(out,size-OFF);copy_extents(src,out,OFF,size,-OFF)
 elif mode=='assemble':
  head=os.open(sys.argv[4],os.O_RDONLY)
  assert os.pwrite(out,os.pread(head,OFF,0),0)==OFF;os.close(head)
  os.ftruncate(out,OFF+size);copy_extents(src,out,0,size,OFF)
 else:raise ValueError(mode)
finally:os.close(out);os.close(src)
