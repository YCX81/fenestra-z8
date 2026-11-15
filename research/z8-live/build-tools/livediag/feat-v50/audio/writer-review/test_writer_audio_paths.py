"""Independent source-level full transaction runs across audio commit/fallback."""
from pathlib import Path
import json
import subprocess
import sys
import hashlib

HERE = Path(__file__).resolve().parent
WRITER = HERE.parents[1] / 'writer'
WORK = Path(sys.argv[1]) if len(sys.argv) > 1 else WRITER / 'nctg-work'
OUT = HERE / 'audio-paths'
OUT.mkdir(exist_ok=True)
source = (WRITER / 'host_writer.c').read_text()
source = source.replace('"work/', '"' + str(WORK) + '/')
source = source.replace('unsigned removed,frames;',
                        'unsigned removed,frames,cancel_late,writes_after_cancel;long moov_at;')
source = source.replace('test*t=v;if(t->fault==2&&n==8', '''test*t=v;
 if(t->cancel_late)t->writes_after_cancel++;
 if(n>=8&&!memcmp((const uint8_t*)p+4,"moov",4))t->moov_at=ftell(t->f[fd]);
 if(t->fault==8&&n>=8&&!memcmp((const uint8_t*)p+4,"meta",4))return -1;
 if(t->fault==9&&n==4&&t->moov_at&&ftell(t->f[fd])==t->moov_at)return -1;
 if(t->fault==7&&n==8&&!memcmp((const uint8_t*)p+4,"udta",4))t->cancel_late=1;
 if(t->fault==2&&n==8''')
source = source.replace('return t->fault==3&&t->frames>0;',
                        'return t->cancel_late||(t->fault==3&&t->frames>0);')
source = source.replace('test*t=v;if(off>t->af)return -1;',
                        'test*t=v;if(t->fault==5&&off)return -1;if(off>t->af)return -1;')
source = source.replace('test*t=v;struct live_audio_info i=',
                        'test*t=v;if(t->fault==6)return 0;struct live_audio_info i=')
source = source.replace('t.pcm=pcm;t.af=pn/4;',
                        't.pcm=pcm;t.af=t.fault==4?0:pn/4;')
source = source.replace('if(t.fault){assert(rc&&cleanup==0',
                        'if((t.fault&&t.fault<4)||t.fault>=6){assert(rc&&cleanup==0')
source = source.replace('free(w);free(dec);',
                        'assert(!t.writes_after_cancel);free(w);free(dec);')
(OUT / 'host_audio_paths.c').write_text(source)
files = ['codec/live_mov.c', 'codec/live_transaction.c', 'codec/live_jpeg.c',
         'codec/preview_writer.c', 'codec/preview_reader.c', 'audio/live_audio_mov.c']
command = ['clang', '-O1', '-g', '-std=c11', '-fsanitize=address,undefined',
           '-fno-sanitize-recover=all', '-Wall', '-Wextra', '-Werror',
           '-Wno-unused-function', str(OUT / 'host_audio_paths.c')]
command += [str(WORK / 'src-fix37' / name) for name in files]
command += ['-o', str(OUT / 'host_audio_paths')]
subprocess.run(command, check=True)
results = []
for fault, name in [(0, 'audio'), (1, 'frame-fail'), (2, 'ncth-write-fail'),
                    (3, 'cancel'), (4, 'silent'), (5, 'audio-read-fallback'),
                    (6, 'audio-trak-fail'), (7, 'cancel-after-ncdt'),
                    (8, 'metadata-tail-write-fail'), (9, 'moov-size-write-fail')]:
    # Only this test's own prior products; writer uses exclusive create.
    for ext in ['MOV', 'MAN', 'JPG']:
        (OUT / (name + '.' + ext)).unlink(missing_ok=True)
    command = [str(OUT / 'host_audio_paths'), str(WRITER / 'input.aus'),
               str(WRITER / 'input.s16'), str(WRITER / 'input-preview.jpg'),
               str(OUT / name), str(fault)]
    result = subprocess.check_output(command, stderr=subprocess.STDOUT, text=True)
    print(name + ': ' + result.strip())
    results.append(dict(name=name, fault=fault, log=result, movie=(OUT / (name + '.MOV')).exists()))
report = dict(source=str(WORK), status='PASS', results=results,
              files={name: hashlib.sha256((WORK / 'src-fix37' / name).read_bytes()).hexdigest() for name in files})
(OUT / 'transactions.json').write_text(json.dumps(report, indent=2) + '\n')
