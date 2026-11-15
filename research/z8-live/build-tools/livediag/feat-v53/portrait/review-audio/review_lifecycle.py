#!/usr/bin/env python3
"""Independent failure schedules over frozen actual PB audio state machine.
Uses reviewed host adapter only for unavailable devices/I/O, and replaces its
callback stub with the actual candidate callback so delayed ACK is exercised.
"""
from pathlib import Path
import ast, hashlib, json, subprocess, tempfile
P=Path(__file__).resolve().parent
A=P.parents[1]/'audio'
S=A/'work/pb-fix37/runtime.c'
s=S.read_text()
assert hashlib.sha256(S.read_bytes()).hexdigest()=='412675da6029c4ca56ce655e55c847be72d64b512d420d7f672a82c4922f1844'
adapter=(A/'test_pb_audio.py').read_text()
template=next(ast.literal_eval(n.value) for n in ast.parse(adapter).body if isinstance(n,ast.Assign) and any(isinstance(x,ast.Name) and x.id=='template' for x in n.targets))
body=s[s.index('int pb_audio_handoff(void)'):s.index('static void audio_callback(')]+s[s.index('static void audio_begin('):s.index('\n/* v37: the preview is inside')]
body=body.replace('base<UINT64_C(0x800000000)||base>UINT64_C(0xa40000000)-PB_AUDIO_RING_BYTES','base!=(uintptr_t)ring_store').replace('for(uint32_t i=0;i<PB_AUDIO_RING_BYTES;i+=64u)__asm__ volatile("dc cvac, %0"::"r"(ring+i):"memory");','').replace('__asm__ volatile("dsb sy":::"memory");','').replace('*(volatile uint16_t *)(uintptr_t)UINT64_C(0xa033868d0)','native_volume').replace('FN(0x403df4e4,uint32_t(*)(uint32_t))','connection_state')
callback=s[s.index('static void audio_callback('):s.index('static void audio_post(')]
template=template.replace('static void audio_callback(uint32_t id,void*ctx){(void)id;(void)ctx;}',callback)
extra=r'''
static PbRuntime* output_setup(unsigned external,unsigned secondary,unsigned volume){
 PbRuntime*r=wait_setup();driver=1;connection4=external;connection6=secondary;native_volume=volume;
 audio_begin(r,7);assert(r->audio_stage==1);r->audio_payload[20]=1;audio_callback(0x43180014,0);audio_step(r);
 assert(r->audio_stage==8&&posts==2&&last_post==0x43180017);
 return r;
}
static void extra_tests(void){
 /* All muted profile combinations preserve 0; unlike p20 (external enable),
  * p28 is a separate reply byte and must be zero before a native reply. */
 for(unsigned a=0;a<3;a++)for(unsigned b=0;b<3;b++){
  PbRuntime*r=output_setup(a,b,0);uint32_t v;
  for(unsigned i=8;i<=24;i+=8){memcpy(&v,r->audio_payload+i,4);assert(v==0);}
  assert(r->audio_payload[4]==(a!=2&&b!=2));assert(r->audio_payload[12]==(b==2));assert(r->audio_payload[20]==(a==2));assert(!r->audio_payload[28]);
  audio_callback(0x4318001b,0);audio_step(r);assert(r->audio_stage==8&&posts==2&&!r->audio_ack);
  audio_callback(0x43180017,0);audio_step(r);assert(r->audio_stage==5&&posts==3&&last_post==0x43180016);
  free(r);free(file);
 }
 /* Cancellation must wait for the output owner response; don't overwrite the
  * native retained pointer. The delayed positive reply may only trigger STOP. */
 for(unsigned reply=0;reply<2;reply++){
  PbRuntime*r=output_setup(2,0,0);uint8_t saved[32];memcpy(saved,r->audio_payload,32);
  assert(!pb_audio_handoff());audio_step(r);assert(r->audio_stage==8&&posts==2&&!memcmp(saved,r->audio_payload,32));
  audio_begin(r,7);assert(posts==2&&r->audio_stage==8&&!memcmp(saved,r->audio_payload,32));
  now+=700000;audio_step(r);assert(r->audio_stage==8&&posts==2); /* exact boundary */
  r->audio_payload[28]=reply;audio_callback(0x43180017,0);audio_step(r);
  assert(r->audio_stage==5&&posts==3&&last_post==0x43180016&&(r->audio_gate&2));
  r->audio_payload[0]=1;audio_callback(0x43180016,0);audio_step(r);assert(!r->audio_stage&&!(r->audio_gate&2)&&pb_audio_handoff());
  free(r);free(file);
 }
 /* After timeout a late ACK must never resurrect playback or give PCM away.
  * Re-entering REC cannot replace this outstanding service's payload. */
 for(unsigned cancel=0;cancel<2;cancel++)for(unsigned reply=0;reply<2;reply++){
  PbRuntime*r=output_setup(0,0,8);if(cancel)assert(!pb_audio_handoff());
  r->audio_pending=1;now+=700001;audio_step(r);assert(r->audio_stage==6&&posts==2&&!r->audio_pending&&(r->audio_gate&2));
  r->audio_payload[28]=reply;audio_callback(0x43180017,0);uint8_t saved[32];memcpy(saved,r->audio_payload,32);
  for(unsigned i=0;i<3;i++){now+=1000000;audio_step(r);audio_begin(r,7);assert(r->audio_stage==6&&posts==2&&(r->audio_gate&2)&&!memcmp(saved,r->audio_payload,32));}
  assert(!pb_audio_handoff());free(r);free(file);
 }
 current_runtime=0;connection4=connection6=0;native_volume=8;
 puts("PASS independent 9 mute/routing/reply-offset cases, 2 cancel-before-ACK schedules, 4 timeout-late-ACK schedules; actual callback and ASan/UBSan");
}
'''
template=template.replace('int main(void){',extra+'\nint main(void){').replace('wait_tests();output_failure_tests();lifecycle_tests();','wait_tests();output_failure_tests();lifecycle_tests();extra_tests();')
with tempfile.TemporaryDirectory(dir=P) as tmp:
 p=Path(tmp);(p/'test.c').write_text(template.replace('__BODY__',body))
 subprocess.run(['clang','-O1','-g','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(A/'work/src-fix37/codec'),str(p/'test.c'),str(A/'work/src-fix37/codec/preview_reader.c'),'-o',str(p/'test')],check=True)
 r=subprocess.run([str(p/'test')],capture_output=True,text=True)
 report=dict(passed=r.returncode==0,source_sha256=hashlib.sha256(S.read_bytes()).hexdigest(),callback='actual candidate; only async service queue is mocked',host_adaptations=['mock device globals/address range/I/O','omit ARM cache maintenance'],extra_cases=15,stdout=r.stdout,stderr=r.stderr)
 (P/'lifecycle.json').write_text(json.dumps(report,indent=2)+'\n');print(r.stdout,end='');print(r.stderr,end='');raise SystemExit(r.returncode)
