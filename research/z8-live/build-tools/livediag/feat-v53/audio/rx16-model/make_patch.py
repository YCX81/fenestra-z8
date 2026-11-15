from pathlib import Path
import hashlib,difflib,json
p=Path(__file__).resolve().parent;b=(p/'baseline.c').read_text();assert hashlib.sha256(b.encode()).hexdigest()=='87c1d8f8594b76068429af6b533d037d0e058dbe75a04913f7629183287f1f41'
s=b.replace('static bool serial_enable(NikonZ8State *s, unsigned ch)',(p/'pcm16.inc').read_text()+'\nstatic bool serial_enable(NikonZ8State *s, unsigned ch)')
needle='    object_class_property_add_str(oc,"serial5-pcm-state",get_serial5_pcm_state,NULL);';assert needle in s
s=s.replace(needle,needle+'\n    object_class_property_add_str(oc,"serial5-pcm16-input",NULL,set_serial5_pcm16_input);\n    object_class_property_add_str(oc,"serial5-pcm16-state",get_serial5_pcm16_state,NULL);')
(p/'candidate.c').write_text(s);patch=''.join(difflib.unified_diff(b.splitlines(True),s.splitlines(True),fromfile='a/hw/arm/nikon-z8-lab.c',tofile='b/hw/arm/nikon-z8-lab.c'));(p/'rx16.patch').write_text(patch)
print(hashlib.sha256(patch.encode()).hexdigest())
