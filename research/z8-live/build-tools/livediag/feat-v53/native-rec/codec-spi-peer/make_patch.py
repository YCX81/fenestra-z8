from pathlib import Path
import difflib,hashlib
P=Path(__file__).resolve().parent
s=(P/'baseline.c').read_text();assert hashlib.sha256(s.encode()).hexdigest()=='87c1d8f8594b76068429af6b533d037d0e058dbe75a04913f7629183287f1f41'
s=s.replace('    char *serial_last_tx;','''    char *serial_last_tx;
    bool codec_peer_enabled, codec_peer_cs;
    unsigned codec_peer_words, codec_peer_pending, codec_peer_valid;
    uint16_t codec_peer_value[2], codec_peer_packet[2];
    uint64_t codec_peer_cs_addr, codec_peer_writes, codec_peer_reads, codec_peer_rejects;
    uint8_t codec_peer_last_descriptor[0x68];
    unsigned codec_peer_last_control, codec_peer_last_queued, codec_peer_last_tx;
    const char *codec_peer_last_fault;''')
s=s.replace('static void serial_abort(', (P/'peer.inc').read_text()+'\nstatic void serial_abort(',1)
s=s.replace('            *serial11 = value;\n            return;','            *serial11 = value;\n            if (ch==10 && value) { codec_peer_receive(s); }\n            return;',1)
s=s.replace('            p->written++;\n            serial_pio_update(s, ch);','            p->written++;\n            codec_peer_transmit(s,ch);\n            serial_pio_update(s, ch);',1)
s=s.replace('        value=z8_mcu_gpio_write(s,addr,value);','        codec_peer_gpio(s,addr,value,size);\n        value=z8_mcu_gpio_write(s,addr,value);',1)
s=s.replace('    object_class_property_add_str(oc, "serial-clock", NULL, set_serial_clock);','''    object_class_property_add_bool(oc,"codec-spi-register-peer",get_codec_peer,set_codec_peer);
    object_class_property_add_str(oc,"codec-spi-register-state",get_codec_peer_state,NULL);
    object_class_property_add_str(oc, "serial-clock", NULL, set_serial_clock);''')
(P/'candidate.c').write_text(s)
(P/'spi-peer.patch').write_text(''.join(difflib.unified_diff((P/'baseline.c').read_text().splitlines(True),s.splitlines(True),fromfile='a/nikon-z8-lab.c',tofile='b/nikon-z8-lab.c')))
print(hashlib.sha256(s.encode()).hexdigest())
