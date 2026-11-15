from pathlib import Path
import subprocess,shlex,json,hashlib
p=Path('/path/to/user/fxfix/v53-shutter/native1280');b=p/'observer2';base=Path('/path/to/user/.local/share/nikon-z8-qemu/rdw')
# Export only the existing read-only clock accessor for the private plugin.
a=json.loads((b/'link-command.json').read_text())+['-Wl,--export-dynamic-symbol=qemu_clock_get_ns'];subprocess.run(a,cwd=base/'build',check=True);(b/'link-command.json').write_text(json.dumps(a,indent=2))
j=json.loads((b/'identity.json').read_text());j['observer_qemu_sha256']=hashlib.sha256((b/'qemu-system-aarch64').read_bytes()).hexdigest();j['read_only_clock_symbol_exported']=True;(b/'identity.json').write_text(json.dumps(j,indent=2)+'\n')
s=Path('/path/to/user/fxfix/v53-native-rec/probe-n2/geometry_observer6.c').read_text();s=s.replace('static FILE *fp;', 'extern int64_t qemu_clock_get_ns(int type); /* actual QEMU_CLOCK_VIRTUAL=1, read-only */\nstatic FILE *fp;').replace('static const uint64_t points[]={','static const uint64_t points[]={0x403f6dc0,')
s=s.replace('fputc(\']\',fp);dump(cpu,"x0_mem"', 'fputc(\']\',fp);fprintf(fp,",\\"virtual_ns\\":%lld",(long long)qemu_clock_get_ns(1));if(pc==0x403f6dc0)dump(cpu,"stop_events",0xa032cd300ull,16);dump(cpu,"x0_mem"')
(b/'geometry_stop.c').write_text(s);cmd=['cc','-O2','-shared','-fPIC','-I'+str(base/'qemu-11.1.1/include/plugins'),str(b/'geometry_stop.c'),'-o',str(b/'geometry_stop.so')]+shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','glib-2.0'],text=True));subprocess.run(cmd,check=True)
s=Path('/path/to/user/fxfix/v53-root/native-f/run-1280.py').read_text().replace('/path/to/user/fxfix/v53-native-rec/probe-n2/geometry_observer6.so',str(b/'geometry_stop.so')).replace('/path/to/user/fxfix/v53-root/native-f/trace-1280.jsonl',str(p/'trace-1280e.jsonl'));(p/'run-1280e.py').write_text(s)
s=Path('/path/to/user/fxfix/v53-root/native-f/launch-1280.sh').read_text().replace('/path/to/user/fxfix/v53-root/native-f/card-1280.raw',str(p/'card-1280e.raw')).replace('/path/to/user/fxfix/v53-root/native-f/run-1280',str(p/'run-1280e')).replace('"$B/rdw/build/qemu-system-aarch64"','"'+str(b/'qemu-system-aarch64')+'"').replace('--virtual-seconds 40','--virtual-seconds 30');(p/'launch-1280e.sh').write_text(s)
print(j)
