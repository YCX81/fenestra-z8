"""External explicit PCM stimulus schedule; never changes guest state/Ready.

RX request bit and a temporarily unavailable DMA bank are different states.
The sample deadline survives native IRQ/rearm gaps. Only a real RX disable
ends an input epoch. Missing input after a saved capture can be declared as
a stimulus gap; no expired block is replayed and no gap is hidden as PCM.
"""
BLOCK_NS = 50_000_000
MAX_LATENESS_NS = 2 * BLOCK_NS


class Feeder:
    def __init__(self):
        self.due = None
        self.index = 0
        self.enabled = False
        self.transitions = []

    def tick(self, ns, state, allow_gap=False):
        enabled = state['serial_config'] & 0x1001 == 0x1000
        if enabled != self.enabled:
            self.transitions.append(dict(virtual_ns=ns, rx_enabled=enabled,
                ready=state['ready'], previous_due=self.due, index=self.index))
            self.enabled = enabled
        if not enabled:
            self.due = None
            return None
        if self.due is None:
            if state['ready']:
                self.due = ns + BLOCK_NS
            return None
        if not state['ready'] or ns < self.due:
            return None
        late = ns - self.due
        if late >= MAX_LATENESS_NS:
            record = dict(action='gap', virtual_ns=ns, due_ns=self.due,
                late_ns=late, first_dropped_index=self.index,
                dropped_blocks=1 + late // BLOCK_NS,
                reason='external stimulus service missed deadlines; no stale block delivered')
            if not allow_gap:
                raise RuntimeError('PCM input deadline missed before capture committed: ' + str(record))
            self.index += record['dropped_blocks']
            self.due = ns + BLOCK_NS
            return record
        action = dict(action='input', virtual_ns=ns, index=self.index,
            due_ns=self.due, late_ns=late)
        self.index += 1
        self.due += BLOCK_NS
        return action
