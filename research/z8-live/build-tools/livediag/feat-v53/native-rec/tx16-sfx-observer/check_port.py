from pathlib import Path
import json,hashlib,re
P=Path(__file__).resolve().parent;D=P/'merged-v2'
old=(D/'baseline.c').read_text();new=(D/'nikon-z9-lab.c').read_text();h=(D/'nikon-private-tx16.h').read_text()
def fn(s,name):
 start=s.rfind('static ',0,s.index(name));p=s.index('{',s.index(name));i=p+1;n=1
 while n:
  n+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[start:i]
unchanged={n:fn(old,n)==fn(new,n) for n in ['pdma_irq_update(', 'pdma_ram_address(', 'pdma_progress(', 'serial5_config(', 'serial_enable(', 'serial5_pcm16_request(', 'set_serial5_pcm16_input(']}
assert all(unchanged.values())
checks={
 'no_host_audio_backend':not any(s in h for s in ['audio_be_', 'SWVoiceOut','set_serial5_audio_output']),
 'private_flags_required':'!(s->serial5_tx16_ring_model || s->serial5_tx16_sfx_model)' in fn(h,'serial5_tx_drain_request('),
 'exact_format_gate':'s->serial5_config[0][0]!=0x450a2000' in h,
 'tuple_bound_before_read':h.index('!serial5_tx_drain_same(s,channel,source,bytes)')<h.index('address_space_read('),
 'actual_read_before_irq':h.index('address_space_read(')<h.index('pdma_progress(s, channel, 0)'),
 'capture_before_irq':h.index('g_byte_array_append(s->serial5_pcm_capture')<h.index('pdma_progress(s, channel, 0)'),
 'dma_write_rearms_before_schedule':'serial5_tx_drain_note_write(s,ch,word);\n    serial5_tx_drain_schedule(s);' in fn(new,'pdma_write('),
 'read_hook_guarded':'!s->serial5_tx_drain_model' in fn(h,'serial5_tx_drain_read('),
 'default_no_timer_returns':'if (!s->serial5_tx_drain_timer)' in fn(h,'serial5_tx_drain_schedule('),
 'master_and_profile_zero_initialized':all(n in new for n in ['bool serial5_tx_drain_model, serial5_tx16_ring_model, serial5_tx16_sfx_model;','object_class_property_add_bool(oc,"serial5-tx16-sfx-model"']),
}
assert all(checks.values())
d={'passed':True,'unchanged_functions':unchanged,'structural_checks':checks,'source_sha256':hashlib.sha256(new.encode()).hexdigest(),'header_sha256':hashlib.sha256(h.encode()).hexdigest(),'scope':'Exact private merged source and header, combined with actual55C cases and ARM format start/stop evidence. Does not replace complete-machine result.'}
(D/'port-tests.json').write_text(json.dumps(d,indent=2));print(json.dumps(d,indent=2))
