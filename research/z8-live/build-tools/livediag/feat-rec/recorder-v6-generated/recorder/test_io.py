#!/usr/bin/env python3
"""Run production recorder C on host with hardware/native boundaries stubbed."""
import re
import subprocess
import sys
from pathlib import Path
import decoder
h = Path(__file__).resolve().parent
b = Path(sys.argv[1]).resolve()
out = b/'recorder-check'
s = (h/'recorder.c').read_text()
s = s.replace('#define F(a,t) ((t)(uintptr_t)UINT64_C(a))', '#define F(a,t) ((t)test_fn(UINT64_C(a)))')
s = re.sub(r'#define R32\(a\).*', '#define R32(a) test_mode()', s)
a, z = s.index('static uint64_t ticks('), s.index('static void give(')
s = s[:a]+'''static uint64_t ticks(void){return test_clock();}
static int take(struct fr_state *s){unsigned zero=0;return __atomic_compare_exchange_n(&s->lock,&zero,1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);}
'''+s[z:]
a, z = s.index('static const void *native_buffer('), s.index('/* RAM phases')
s = s[:a]+'''static const void *native_buffer(struct fr_state *s,uint32_t n){(void)n;return s->io;}
'''+s[z:]
a, z = s.index('static int sleepable('), s.index('static int pulse(')
s = s[:a]+'''static int sleepable(void){return test_sleepable();}
static uint64_t frequency(void){return test_frequency();}
'''+s[z:]
s = s.replace('((volatile uint8_t *)(uintptr_t)UINT64_C(0xa02e98480))[(uint32_t)r[19]]', 'test_map((uint32_t)r[19])')
s += '\nconst char fr_exit_return[1]={0},fr_nr_lr_a[1]={0},fr_nr_lr_b[1]={0};\nuint64_t fr_exit_scoped(struct fr_state *s){(void)s;return 0;}\n'
s = '''#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
static void *test_fn(unsigned long long);
static unsigned test_mode(void),test_sleepable(void),test_map(unsigned);
static uint64_t test_clock(void),test_frequency(void);
'''+s+(h/'host_io_cases.c.inc').read_text()
target = b/'recorder/host_io_test.c'
target.write_text(s)
subprocess.run(['clang','-U__aarch64__','-std=c11','-pthread','-Wall','-Wextra','-Werror',
                '-I',str(b/'ui/core/include'),str(target),'-o',str(out/'host_io_test')],check=True)
subprocess.run([str(out/'host_io_test')],check=True,cwd=out)
rows, errors = decoder.decode((out/'recorder-v4-fixture.bin').read_bytes())
assert not errors and [r['事件号'] for r in rows] == [0], (rows, errors)
print('PASS: actual C transactions/identity/timer/protocol with native boundaries stubbed; 200 CAS races; zero recorder I/O at/after entry; v4 disk fixture decoded')
print('LIMIT: host stubs, not AArch64 native task scheduling, GPIO, filesystem or camera execution')
