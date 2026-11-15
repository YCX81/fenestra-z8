from pathlib import Path
import re, json, hashlib, subprocess

P = Path(__file__).resolve().parent
D = P / 'rdw-v3'
h = (D / 'nikon-z8-audio-tx.h').read_text()
m = (P / 'nikon-private-tx16.h').read_text().replace('NikonZ9State', 'NikonZ8State').replace('NIKON_Z9_LAB', 'NIKON_Z8_LAB')

def fn(text, name):
    match = re.search(r'^static [^;{\n]*\b' + re.escape(name) + r'\([^;{]*\)\s*\{', text, re.M)
    assert match, name
    start = match.start()
    at = match.end()
    depth = 1
    while depth:
        depth += (text[at] == '{') - (text[at] == '}')
        at += 1
    return text[start:at]

names = ['serial5_tx_drain_request', 'serial5_tx_drain_same', 'serial5_tx_drain_note_write',
         'serial5_tx_drain_tick', 'serial5_tx_clock_delay', 'serial5_tx_drain_schedule',
         'serial5_tx_drain_read']
same = {}
for name in names:
    a, b = fn(m, name), fn(h, name)
    if name == 'serial5_tx_drain_request':
        # Merged has no old TX backend: retain its stricter master-only rejection.
        a = a.replace(' || !(s->serial5_tx16_ring_model || s->serial5_tx16_sfx_model)', '', 1)
    same[name] = a == b
assert all(same.values()), same
assert fn(m, 'set_serial5_tx_counter_clock') == fn(h, 'set_serial5_tx_counter_clock')

# Replace all model implementations, not just the changed scheduler. Leaves
# model RAM/IRQ/timer and host-output shims explicit in the existing harness.
s = (P / 'test_counter.c').read_text()
for name in names:
    s = s.replace(fn(s, name), fn(h, name), 1)
(D / 'test_rdw.c').write_text(s)
inc = P.parents[1] / 'integration-f/work/src-fix37/codec'
cmd = ['clang', '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
       '-I' + str(inc), str(D / 'test_rdw.c'), '-o', str(D / 'test_rdw')]
subprocess.run(cmd, check=True, capture_output=True)
r = subprocess.run([str(D / 'test_rdw')], text=True, capture_output=True)
(D / 'test-stdout.txt').write_text(r.stdout)
(D / 'test-stderr.txt').write_text(r.stderr)
assert r.returncode == 0, r.stderr[-2000:]
result = json.loads(r.stdout)
result.update(shared_actual_functions=same, counter_setter_identical=True,
              header_sha256=hashlib.sha256(h.encode()).hexdigest(),
              differences='rdw retains prior master-only legacy TX and optional S32 host backend; narrow16 rejects host output. Merged requires a private ring/SFX flag.',
              scope='Exact rdw compiled request/tick/schedule/read implementations, real F and fixed PB frame functions, ASan/UBSan. Explicit RAM/timer/DMA/render leaves; no acoustic claim.')
(D / 'port-tests.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result, indent=2))
