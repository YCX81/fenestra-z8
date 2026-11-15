from pathlib import Path
import hashlib,difflib
P=Path(__file__).resolve().parent;s=(P/'baseline.c').read_text();assert hashlib.sha256(s.encode()).hexdigest()=='9544493351aa86ce2ed22a73b8acd3647f1722d78d8e539a6761727a2eaedaac'
def one(old,new):
 global s
 assert s.count(old)==1,(old,s.count(old));s=s.replace(old,new)
one('    char *serial_last_tx;','''    char *serial_last_tx;
    bool codec_peer_enabled, codec_peer_cs;
    unsigned codec_peer_words, codec_peer_pending, codec_peer_valid;
    uint16_t codec_peer_value[2], codec_peer_packet[2];
    uint64_t codec_peer_cs_addr, codec_peer_writes, codec_peer_reads, codec_peer_rejects;
    uint8_t codec_peer_last_descriptor[0x68];
    unsigned codec_peer_last_control, codec_peer_last_queued, codec_peer_last_tx;
    const char *codec_peer_last_fault;''')
one('static void serial_abort(', (P.parent/'peer.inc').read_text()+'\nstatic void serial_abort(')
one('            *serial11 = value;\n            return;','            *serial11 = value;\n            if (ch==10 && value) { codec_peer_receive(s); }\n            return;')
one('            p->written++;\n            serial_pio_update(s, ch);','            p->written++;\n            codec_peer_transmit(s,ch);\n            serial_pio_update(s, ch);')
one('        value = z8_mcu_gpio_write(s, addr, value);','        codec_peer_gpio(s,addr,value,size);\n        value = z8_mcu_gpio_write(s, addr, value);')
one('    object_class_property_add_str(oc, "serial-clock", NULL, set_serial_clock);','''    object_class_property_add_bool(oc,"codec-spi-register-peer",get_codec_peer,set_codec_peer);
    object_class_property_add_str(oc,"codec-spi-register-state",get_codec_peer_state,NULL);
    object_class_property_add_str(oc, "serial-clock", NULL, set_serial_clock);''')
r=(P.parents[2]/'audio/rx16-model/rx16.patch').read_text().splitlines();added='\n'.join(l[1:] for l in r if l.startswith('+') and not l.startswith('+++'))+'\n';code,props=added.split('    object_class_property_add_str(oc,"serial5-pcm16-input"',1)
one('static bool serial_enable(',code+'\nstatic bool serial_enable(')
one('    object_class_property_add_str(oc, "serial5-pcm-state", get_serial5_pcm_state, NULL);','    object_class_property_add_str(oc, "serial5-pcm-state", get_serial5_pcm_state, NULL);\n    object_class_property_add_str(oc,"serial5-pcm16-input"'+props.rstrip())
(P/'candidate.c').write_text(s)
(P/'merged-spi-rx.patch').write_text(''.join(difflib.unified_diff((P/'baseline.c').read_text().splitlines(True),s.splitlines(True),fromfile='a/hw/arm/nikon-z9-lab.c',tofile='b/hw/arm/nikon-z9-lab.c')))
print(hashlib.sha256(s.encode()).hexdigest())
