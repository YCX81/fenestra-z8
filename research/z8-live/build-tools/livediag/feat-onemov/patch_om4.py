"""v56 = om3 + the v53 shutter-sound microphone hand-off switched off (never holds a sound request,
never pauses the microphone). On hardware it produced no sound and correlates with lost audio / end-of-playback freezes."""
import sys,subprocess
from pathlib import Path
W=Path(sys.argv[1]);subprocess.run([sys.executable,str(Path(__file__).with_name('patch_om3.py')),str(W)],check=True)
p=W/'src-fix37/audio/live_shutter_z8.c';t=p.read_text()
a=t.index("static SH int allowed(struct live_runtime*r){");b=t.index("static SH int sound_idle(void)")
p.write_text(t[:a]+"static SH int allowed(struct live_runtime*r){(void)r;return 0;} /* v56: hand-off disabled */\n"+t[b:])
print('om4 patched')
