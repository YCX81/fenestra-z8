"""Compile one private machine object and relink; shared source/build stay intact."""
import difflib
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import os

ROOT = Path('/path/to/user/.local/share/nikon-z8-qemu/rdw')
PEER = os.environ.get('EYE_PEER') == '1'
OUT = Path('/path/to/user/fxfix/v53-evf/model2' if PEER else '/path/to/user/fxfix/v53-evf/model')
if os.environ.get('EYE_MODEL_DIR'):
    name = os.environ['EYE_MODEL_DIR']
    assert name.isalnum() and name.startswith('model')
    OUT = OUT.parent / name
OUT.mkdir(parents=True, exist_ok=True)
src = ROOT / 'qemu-11.1.1/hw/arm/nikon-z8-lab.c'
old = src.read_text()
s = old
def replace(a, b):
    global s
    assert s.count(a) == 1, (a[:100], s.count(a))
    s = s.replace(a, b)

replace('    qemu_irq capture_gpio_irq[2];', '''    qemu_irq capture_gpio_irq[2];
    /* v53 diagnostic: externally supplied physical-level candidate only. */
    qemu_irq eye_gpio_irq;
    bool eye_gpio_enabled, eye_gpio_level;
    uint64_t eye_gpio_edges, eye_gpio_acks;''')
replace('static void capture_gpio_update(NikonZ8State *s)', '''/* Original MAIN maps logical GPIO44 to bank3 bit13, IRQ bank1 bit4,
 * INTID68. Native 40c15e80 selects type0, initially opposite polarity;
 * 40c16358 acknowledges then flips polarity for the next physical edge.
 * This fixture does not set any guest GUI/output/state field. */
static void eye_gpio_update(NikonZ8State *s)
{
    if (s->eye_gpio_irq) {
        qemu_set_irq(s->eye_gpio_irq, s->eye_gpio_enabled &&
            !!(s->gpio_irq_config[1][2] & ~s->gpio_irq_config[1][0] & 0x10));
    }
}
static void set_eye_gpio_input(Object *obj, const char *value, Error **errp)
{
    NikonZ8State *s = NIKON_Z8_LAB(obj);
    if (!s->bootstrap_fixtures || !s->eye_gpio_irq || !value ||
        strlen(value) != 1 || (value[0] != '0' && value[0] != '1')) {
        error_setg(errp, "eye-gpio-input requires realized bootstrap and 0/1");
        return;
    }
    bool next = value[0] == '1';
    bool old = s->eye_gpio_enabled ? s->eye_gpio_level :
               !!(s->syscon[0x100c / 4] & 0x2000);
    s->eye_gpio_enabled = true;
    s->eye_gpio_level = next;
    if (next != old && !!(s->gpio_irq_config[1][5] & 0x10) == next &&
        (s->gpio_irq_config[1][6] & 0x10)) {
        s->gpio_irq_config[1][2] |= 0x10;
        s->eye_gpio_edges++;
    }
    fprintf(stderr, "Z8_EYE_INPUT ns=%"PRId64" old=%u level=%u matched=%"PRIu64
        " mask=%x polarity=%x type=%x pending=%x; external candidate only\\n",
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), old, next, s->eye_gpio_edges,
        s->gpio_irq_config[1][0], s->gpio_irq_config[1][5],
        s->gpio_irq_config[1][6], s->gpio_irq_config[1][2]);
    eye_gpio_update(s);
}
static char *get_eye_gpio_state(Object *obj, Error **errp)
{
    NikonZ8State *s = NIKON_Z8_LAB(obj);
    return g_strdup_printf("{\\"enabled\\":%s,\\"level\\":%u,\\"edges\\":%"PRIu64
        ",\\"acks\\":%"PRIu64",\\"mask\\":%u,\\"polarity\\":%u,\\"type\\":%u,"
        "\\"pending\\":%u,\\"physical_timing_verified\\":false}",
        s->eye_gpio_enabled ? "true" : "false", s->eye_gpio_level,
        s->eye_gpio_edges, s->eye_gpio_acks, s->gpio_irq_config[1][0],
        s->gpio_irq_config[1][5], s->gpio_irq_config[1][6], s->gpio_irq_config[1][2]);
}

static void capture_gpio_update(NikonZ8State *s)''')
replace('        return z8_mcu_gpio_read(s,addr,s->syscon[(addr - 0x0d150000) / 4]);', '''        uint32_t value = z8_mcu_gpio_read(s,addr,s->syscon[(addr - 0x0d150000) / 4]);
        if (s->eye_gpio_enabled && addr == 0x0d15100c) {
            value = (value & ~0x2000u) | (s->eye_gpio_level ? 0x2000u : 0);
        }
        return value;''')
replace('            s->gpio_irq_config[(addr >> 12) & 1][2] &= ~value;', '''            if (s->eye_gpio_enabled && ((addr >> 12) & 1)) {
                s->eye_gpio_acks += !!(s->gpio_irq_config[1][2] & value & 0x10);
            }
            s->gpio_irq_config[(addr >> 12) & 1][2] &= ~value;''')
replace('        capture_gpio_update(s);\n        ccomp_gpio_update(s);', '        eye_gpio_update(s);\n        capture_gpio_update(s);\n        ccomp_gpio_update(s);')
replace('    z8_pcie_init(s, mem, gic);', '    z8_pcie_init(s, mem, gic);\n    s->eye_gpio_irq = qdev_get_gpio_in(gic, 68 - 32);')
anchor='    object_class_property_add_str(oc, "capture-gpio-state", get_capture_gpio_state, NULL);'
replace(anchor, anchor + '''
    object_class_property_add_str(oc, "eye-gpio-input", NULL, set_eye_gpio_input);
    object_class_property_add_str(oc, "eye-gpio-state", get_eye_gpio_state, NULL);''')

if PEER:
    replace('    bool eye_gpio_enabled, eye_gpio_level;', '''    bool eye_gpio_enabled, eye_gpio_level, eye_peer_model;
    uint8_t eye_peer_regs[256];
    uint64_t eye_peer_monitors, eye_peer_writes;''')
    replace('static bool orientation_transfer(NikonZ8State *s, unsigned ch)', '''/* Eye initialization at 40c94848 writes only these identified registers
 * on physical controller3/address38. No ADC/proximity data is fabricated. */
static bool eye_peer_transfer(NikonZ8State *s, unsigned ch)
{
    uint32_t *r = s->i2c[ch];
    unsigned n = s->i2c_tx_count[ch];
    if (r[0] & 1) { return false; } /* no unprovided sensor sample */
    if (!n) {
        s->i2c_monitor[ch] = true;
        s->eye_peer_monitors++;
        r[4] |= 0x10;
        return true;
    }
    unsigned start = s->i2c_tx[ch][0];
    fprintf(stderr, "Z8_EYE_PEER_REQUEST control=%x reg=%x tx=%u rx=%u first=%x\\n", r[0],start,n,r[5],n>1?s->i2c_tx[ch][1]:0);
    if (n < 2 || n > 6 || !((start == 0x40 && n == 2) ||
        (start == 0x41 && (n == 2 || n == 4)) ||
        (start == 0x43 && n == 2) || (start == 0x4a && n == 6) ||
        (start == 0x53 && n == 3))) { return false; }
    for (unsigned i = 1; i < n; i++) { s->eye_peer_regs[start+i-1] = s->i2c_tx[ch][i]; }
    s->eye_peer_writes++;
    fprintf(stderr, "Z8_EYE_PEER ns=%"PRId64" reg=%x bytes=%u first=%x; bounded original config write\\n",
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), start, n-1, s->i2c_tx[ch][1]);
    s->i2c_tx_count[ch] = 0;
    r[4] |= 1;
    return true;
}

static bool orientation_transfer(NikonZ8State *s, unsigned ch)''')
    replace('            } else if (r[0] & 0x20) {\n                s->i2c_monitor[ch] = true;', '''            } else if (s->eye_peer_model && ch == 3 && value == 0x38) {
                if (!eye_peer_transfer(s, ch)) { return false; }
            } else if (r[0] & 0x20) {
                s->i2c_monitor[ch] = true;''')
    replace('static void eye_gpio_update(NikonZ8State *s)', '''static bool get_eye_peer_model(Object *obj, Error **errp)
{
    return NIKON_Z8_LAB(obj)->eye_peer_model;
}
static void set_eye_peer_model(Object *obj, bool value, Error **errp)
{
    NikonZ8State *s = NIKON_Z8_LAB(obj);
    if (s->eye_gpio_irq) { error_setg(errp, "eye-peer-model immutable after realization"); return; }
    s->eye_peer_model = value;
}
static void eye_gpio_update(NikonZ8State *s)''')
    replace('    object_class_property_add_str(oc, "eye-gpio-state", get_eye_gpio_state, NULL);', '''    object_class_property_add_str(oc, "eye-gpio-state", get_eye_gpio_state, NULL);
    object_class_property_add_bool(oc, "eye-peer-model", get_eye_peer_model, set_eye_peer_model);''')

private = OUT / src.name
private.write_text(s)
(OUT/'eye-model.patch').write_text(''.join(difflib.unified_diff(old.splitlines(True), s.splitlines(True), fromfile=str(src), tofile=str(private))))
build = ROOT / 'build'
entry = next(x for x in json.loads((build/'compile_commands.json').read_text()) if x['file'].endswith('/nikon-z8-lab.c'))
cmd = shlex.split(entry['command'])
cmd[cmd.index('-c')+1] = str(private)
obj = OUT/'nikon-z8-lab.o'
for flag, value in [('-o',str(obj)),('-MQ',str(obj)),('-MF',str(obj)+'.d')]: cmd[cmd.index(flag)+1]=value
cmd += ['-iquote', str(src.parent)]
(OUT/'compile-command.json').write_text(json.dumps(cmd,indent=2))
subprocess.run(cmd,cwd=build,check=True)

lines=(build/'build.ninja').read_text().splitlines()
i=next(i for i,x in enumerate(lines) if x.startswith('build qemu-system-aarch64:'))
inputs=shlex.split(lines[i].split(': cpp_LINKER_RSP ',1)[1].split(' |',1)[0])
assert inputs.count(entry['output'])==1
inputs[inputs.index(entry['output'])]=str(obj)
args=shlex.split(lines[i+1].split(' = ',1)[1])
binary=OUT/'qemu-system-aarch64-eye'
cmd=['c++','-m64','-o',str(binary),*inputs,*args]
(OUT/'link-command.json').write_text(json.dumps(cmd,indent=2))
subprocess.run(cmd,cwd=build,check=True)
sha=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest()
manifest=dict(baseline_source=str(src),baseline_source_sha256=sha(src),
    private_source_sha256=sha(private),baseline_binary_sha256=sha(build/'qemu-system-aarch64'),
    private_binary_sha256=sha(binary),firmware_unchanged=True,eye_config_peer=PEER,
    scope='External GPIO bank3 bit13, edge polarity/type/mask and W1C on IRQ bank1 bit4 INTID68 only; no guest RAM writes')
(OUT/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps(manifest,indent=2))
