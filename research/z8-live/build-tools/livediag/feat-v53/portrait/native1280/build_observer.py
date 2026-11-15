from pathlib import Path
import json,shlex,subprocess,hashlib,re,difflib
base=Path('/path/to/user/.local/share/nikon-z8-qemu/rdw');b=base/'build';src=base/'qemu-11.1.1/hw/arm';out=Path('/path/to/user/fxfix/v53-shutter/native1280/observer');out.mkdir(exist_ok=True)
for name in ['nikon-z8-lab.c','nikon-z8-wave5.h']:(out/name).write_bytes((src/name).read_bytes())
p=out/'nikon-z8-wave5.h';old=p.read_text();s=old
start=s.index('static bool wave5_decode_picture(');end=s.index('/* A result remains owned',start);part=s[start:end]
part=part.replace('WlVideoDecode *d=i->decoder;', 'WlVideoDecode *d=i->decoder;\n    fprintf(stderr,"W5AUDIT_PICTURE outputs=%u rd=%08x crd=%08x cwr=%08x opt=%08x end=%u drained=%u queued=%u result=%u seq=%u configured=%u buffers=%u\\n",(unsigned)d->outputs,i->decode_rd,d->command_rd,d->command_wr,d->command_options,d->stream_end,d->drained,g_queue_get_length(&d->frames),d->picture_result,i->sequence_result,i->configured,i->buffers);')
i=0
def fail(m):
 global i;i+=1
 return f'fprintf(stderr,"W5AUDIT_FAIL site={i} outputs=%u rd=%08x wr=%08x end=%u drained=%u failed=%u queued=%u\\n",(unsigned)d->outputs,i->decode_rd,d->write_bus,d->stream_end,d->drained,d->failed,g_queue_get_length(&d->frames));return false;'
part=re.sub(r'return false;',fail,part);s=s[:start]+part+s[end:]
s=s.replace('if(ok) { ok=wl_video_dec_feed(d,data,bytes,&consumed); }','''if(ok && d->outputs>=14 && d->outputs<=17) { static unsigned audit_dump_n;char name[256];snprintf(name,sizeof name,"/path/to/user/fxfix/v53-shutter/native1280/observer/ring-%u.bin",audit_dump_n++);FILE *f=fopen(name,"wb");if(f){fwrite(data,1,bytes,f);fclose(f);} }
    if(ok) { ok=wl_video_dec_feed(d,data,bytes,&consumed); }
    fprintf(stderr,"W5AUDIT_FEED outputs=%u rd=%08x wr=%08x avail=%u consumed=%u ok=%u failed=%u queued=%u\\n",(unsigned)d->outputs,rd,wr,bytes,consumed,ok,d->failed,g_queue_get_length(&d->frames));''')
p.write_text(s);(out/'observer.patch').write_text(''.join(difflib.unified_diff(old.splitlines(True),s.splitlines(True),fromfile='a/nikon-z8-wave5.h',tofile='b/nikon-z8-wave5.h')))
j=json.loads((b/'compile_commands.json').read_text());cmd=next(x for x in j if x['file'].endswith('/hw/arm/nikon-z8-lab.c'))
a=shlex.split(cmd['command']);a+=['-iquote',str(src)]
for flag in ['-o','-MQ','-MF','-c']:
 k=a.index(flag);a[k+1]=str(out/('nikon-z8-lab.c' if flag=='-c' else 'observer.o.d' if flag=='-MF' else 'observer.o'))
(out/'compile-command.json').write_text(json.dumps(a,indent=2));subprocess.run(a,cwd=b,check=True)
lines=(b/'build.ninja').read_text().splitlines();n=next(i for i,l in enumerate(lines)if l.startswith('build qemu-system-aarch64:'))
inp=shlex.split(lines[n].split(': cpp_LINKER_RSP ',1)[1].split(' |')[0]);assert inp.count(cmd['output'])==1
inp=[str(out/'observer.o')if x==cmd['output']else x for x in inp]
links=shlex.split(lines[n+1].split(' = ',1)[1]);link=['c++','-m64','-o',str(out/'qemu-system-aarch64')]+inp+links
(out/'link-command.json').write_text(json.dumps(link,indent=2));subprocess.run(link,cwd=b,check=True)
record={'original_qemu_sha256':hashlib.sha256((b/'qemu-system-aarch64').read_bytes()).hexdigest(),'observer_qemu_sha256':hashlib.sha256((out/'qemu-system-aarch64').read_bytes()).hexdigest(),'base_header_sha256':hashlib.sha256(old.encode()).hexdigest(),'observer_header_sha256':hashlib.sha256(s.encode()).hexdigest(),'failure_sites':i,'scope':'read-only logging, unchanged decisions/IRQs/queues; bounded raw ring snapshots at outputs14..17'}
(out/'identity.json').write_text(json.dumps(record,indent=2)+'\n');print(record)
