"""Socket-free local QEMU IPC using named FIFO character devices.

Opt-in only; firmware and protocol traffic stay unchanged.
"""
import json
import os
import select
import subprocess
import time
from pathlib import Path

class PipeIO:
    def __init__(self, base, timeout=10):
        self.rd = os.open(str(base)+'.out', os.O_RDWR | os.O_NONBLOCK)
        self.wr = os.open(str(base)+'.in', os.O_RDWR | os.O_NONBLOCK)
        self.timeout = timeout
        self.closed = False
    def recv(self, count):
        if not select.select([self.rd], [], [], self.timeout)[0]:
            raise TimeoutError('QEMU FIFO read timeout')
        return os.read(self.rd, count)
    def write(self, data):
        total = 0
        while total < len(data):
            if not select.select([], [self.wr], [], self.timeout)[1]:
                raise TimeoutError('QEMU FIFO write timeout')
            total += os.write(self.wr, data[total:])
        return total
    sendall = write
    def readline(self):
        result = bytearray()
        while not result.endswith(b'\n'):
            result.extend(self.recv(1))
        return bytes(result)
    def close(self):
        if not self.closed:
            os.close(self.rd); os.close(self.wr); self.closed = True


def install():
    import guest_transport
    from probe import QMP
    class PipeQMP(QMP):
        def __init__(self, path):
            self.sock = self.fp = PipeIO(path)
            self.events = []
            self.greeting = json.loads(self.fp.readline())
            self.command('qmp_capabilities')
    def enter(self):
        try:
            for flag, name in (('-qmp','qmp'),('-qtest','qtest'),('-gdb','gdb')):
                base = self.runtime / name
                os.mkfifo(str(base)+'.in'); os.mkfifo(str(base)+'.out')
                self.command[self.command.index(flag)+1] = 'pipe:'+str(base)
            with self.log_path.open('wb') as log:
                self.proc = subprocess.Popen(self.command, stdout=subprocess.DEVNULL, stderr=log)
            self.qmp = PipeQMP(self.runtime/'qmp')
            self.qt = self.fp = PipeIO(self.runtime/'qtest')
            if self.accelerator != 'qtest':
                self.gdb = PipeIO(self.runtime/'gdb')
            if self.intercept_irq:
                children = self.qmp.command('qom-list', path='/machine/unattached')
                gics = [x for x in children if x['type'] == 'child<arm-gicv3>']
                if len(gics) != 1:
                    raise RuntimeError('GIC object not unique')
                self.qt_command('irq_intercept_in /machine/unattached/'+gics[0]['name'])
            if self.accelerator != 'qtest':
                self.rsp('Z1,40160000,4')
            self.data['local_transport'] = 'named-pipe'
            return self
        except BaseException as exc:
            self.__exit__(type(exc), exc, None)
            raise
    guest_transport.Transport.__enter__ = enter
