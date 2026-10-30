"""Compare consecutive screenshots of a run: changed pixels in the content area."""
import sys
from pathlib import Path
from PIL import Image, ImageChops
d=Path(sys.argv[1]); names=sys.argv[2:]
prev=None
for n in names:
    im=Image.open(d/n).convert('RGB').crop((110,90,1020,680))
    if prev is not None:
        px=sum(1 for v in ImageChops.difference(im,prev).convert('L').getdata() if v>16)
        print(n,'changed',px,'of',im.width*im.height)
    prev=im
