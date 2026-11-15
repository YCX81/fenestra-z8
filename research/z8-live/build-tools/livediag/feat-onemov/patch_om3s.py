"""TEST ONLY: om3 with the recorded-audio branch disabled, to exercise the silent-track fallback."""
import sys,subprocess
from pathlib import Path
W=Path(sys.argv[1]);subprocess.run([sys.executable,str(Path(__file__).with_name('patch_om3.py')),str(W)],check=True)
p=W/'src-fix37/runtime.c';t=p.read_text();a=" if(audio==2){still.audio_get=live_audio_ticket_get;";assert t.count(a)==1
p.write_text(t.replace(a," if(audio==99){still.audio_get=live_audio_ticket_get;"))
