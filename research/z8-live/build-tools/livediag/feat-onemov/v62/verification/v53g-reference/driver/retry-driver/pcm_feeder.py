"""External explicit PCM stimulus schedule; never changes guest state/Ready.

RX request bit and a temporarily unavailable DMA bank are different states.
The sample deadline survives native IRQ/rearm gaps. Only a real RX disable
ends an input epoch. Missing input after a saved capture can be declared as
a stimulus gap; no expired block is replayed and no gap is hidden as PCM.
"""
# QEMU ARM uses floor(1e9/24MHz)=41ns counter ticks. Guest microseconds
# are QOM ns/984: this explicit fixture follows the guest sample-clock domain.
# No CPU timer, CNTFRQ, firmware freshness or model tuple gate is modified.
BLOCK_NS = 50_000 * 984
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


def cancelled_by_rx_stop(error, before, after):
    """Only the non-consuming model admission rejection plus observed RX off."""
    exact = 'PCM16 input does not match original enabled nonoverlapping9600B RX bank'
    payload = error.args[0] if error.args else None
    return (isinstance(payload, dict) and payload.get('class') == 'GenericError'
        and payload.get('desc') == exact and before.get('ready') is True
        and after.get('ready') is False and not (after['serial_config'] & 0x1000)
        and after['blocks'] == before['blocks']
        and after['irq_assertions'] == before['irq_assertions'])
