"""Passive runtime observation in the native MCU run; no guest writes."""
import json
from pathlib import Path

OFF = json.loads(Path(__file__).with_name('offsets.json').read_text())

def sample(g):
    rd = lambda a, n: int.from_bytes(g.read_bytes(a, n), 'little')
    valid = lambda a, n: 0xa00000000 <= a <= 0xa40000000 - n and a % 8 == 0
    result = {}
    try:
        root = rd(0xa032b50d8, 8)
        if not valid(root, 664 * 96): return dict(unavailable='page_root', value=hex(root))
        core = rd(root + 663 * 96 + 80, 8)
        if not valid(core, 2536): return dict(unavailable='core', value=hex(core))
        reg = rd(core + 2528, 8)
        if not valid(reg, 24) or rd(reg, 4) != 0x47525846: return dict(unavailable='registry', value=hex(reg))
        ui = rd(reg + 8, 8)
        if not valid(ui, 16): return dict(unavailable='ui', value=hex(ui))
        engine = rd(ui + 8, 8)
        if not valid(engine, OFF['struct_bytes']): return dict(unavailable='runtime', value=hex(engine))
        data = g.read_bytes(engine, OFF['struct_bytes'])
        assert int.from_bytes(data[4:8], 'little') == OFF['struct_bytes']
        result.update(runtime=hex(engine), fields={})
        for name in ['requested', 'task_started', 'task_error', 'capture.phase', 'capture.irq_seq',
                     'stream.encoded', 'stream.error', 'stream.job', 'stream.cleanup_failed']:
            offset = OFF[name]
            width = 8 if name in ['capture.irq_seq', 'stream.encoded'] else 4
            result['fields'][name] = int.from_bytes(data[offset:offset+width], 'little')
        result['w5_vdi'] = hex(rd(0xa02d3d980, 8))
        result['native_view'] = hex(rd(0xa032280c8, 8))
        return result
    except Exception as e:
        result['observation_error'] = repr(e)
        return result
