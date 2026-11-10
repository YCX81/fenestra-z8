"""Offline stand-in for a Z8 running Fenestra: answers FXP2 HELLO/SUBMIT/FETCH with
the same frame layout as the camera core (src/ptp.c, native_rtos_monitor.c) and
simulates just enough of `audiodbg` to exercise z8_audio_probe.py. Not a model of
the camera's audio hardware."""
import struct
import zlib

HEADER = struct.Struct('<4sHHQIIII')
STATE, BUFFER_PTR, DEPTH_ARG, DEPTH_LATCHED, FILE_NO = (
    0xa02d4cb48, 0xa038fef78, 0xa038ff694, 0xa038ff690, 0xa02920560)
COUNTER = 0xa038ff3d0  # pretend capture progress word inside the sound-test window
CAPS = (1 << 0) | (1 << 1) | (1 << 5) | (1 << 6) | (1 << 8)  # 355, as the bridge64 core


class FakeCamera:
    def __init__(self, help_lists_audiodbg=True):
        self.mem = {}
        self.session = 0x1122334455667788
        self.pending = None
        self.job = 0
        self.phase = 0
        self.output = b''
        self.recording = False
        self.help_lists_audiodbg = help_lists_audiodbg
        self.lines = []
        self.posted = []
        self.put(STATE, struct.pack('<I', 1))
        self.put(0xa031f8bb8, struct.pack('<QQQ', 0xa170fe400, 0, 5))
        self.put(BUFFER_PTR, struct.pack('<Q', 0xa36ee2400))

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False

    def put(self, address, data):
        for i, b in enumerate(data):
            self.mem[address + i] = b

    def get(self, address, count):
        if self.recording:
            word = int.from_bytes(bytes(self.mem.get(COUNTER + i, 0) for i in range(4)), 'little')
            self.put(COUNTER, struct.pack('<I', (word + 0x3000) & 0xffffffff))
        return bytes(self.mem.get(address + i, 0) for i in range(count))

    def nikon_read(self, opcode, params):
        prohibit = 0 if getattr(self, 'app_mode', 0) else 1 << 14
        return 0x2001, struct.pack('<I', prohibit)

    def nikon_command(self, opcode, params=()):
        """Nikon remote ops: REC start/end toggle a pretend Wave5 bring-up."""
        self.lines.append(f'nikon {opcode:#06x}')
        if opcode == 0x9435:
            self.app_mode = params[0]
        if opcode == 0x920a:
            self.put(0xa02d3d980, struct.pack('<Q', 0x16010000))
        if opcode == 0x920b:
            self.put(0xa02d3d980, bytes(8))
        return 0x2001

    # transport API used by ptp_client.Client
    def read(self, opcode, params, maximum):
        if opcode == 0x95e0:
            raw = bytearray(64)
            raw[:4] = b'FXP2'
            struct.pack_into('<IQ4I', raw, 4, 2, self.session, 1024, CAPS, 311, 1)
            struct.pack_into('<II', raw, 32, 1, 3 | 4)
            return bytes(raw)
        if opcode == 0x95e2:
            if self.pending is None:
                raise RuntimeError('fetch without submit')
            op, rid, status, data = self.pending
            self.pending = None
            raw = HEADER.pack(b'FXP2', 2, op, self.session, rid, len(data), status, 0) + data
            return raw[:28] + struct.pack('<I', zlib.crc32(raw)) + raw[32:]
        raise RuntimeError(f'unexpected read {opcode:#x}')

    def write(self, opcode, params, payload):
        magic, ver, op, session, rid, n, _, _ = HEADER.unpack_from(payload)
        assert (magic, ver, session, n) == (b'FXP2', 2, self.session, len(payload) - 32)
        data = payload[32:]
        try:
            out = self.backend(op, data)
            status = 0x80000000
        except ValueError:
            out, status = b'', 0x80000001
        self.pending = (op, rid, status, out)

    def backend(self, op, data):
        if op == 9:
            raw = bytearray(256)
            raw[:4] = b'FXI1'
            struct.pack_into('<10I', raw, 4, 1, 256, 2, 1, 1, 1, CAPS, 311, 1, 3)
            for start in (64, 96, 128):
                raw[start:start + 32] = b'\x5a' * 32
            raw[160:168] = b'Nikon Z8'
            raw[208:214] = b'C 3.11'
            return bytes(raw)
        if op != 7:
            raise ValueError('unsupported')
        sub = struct.unpack_from('<I', data)[0]
        body = data[4:]
        if sub == 0:
            out = bytearray(96)
            out[:4] = b'FXT1'
            struct.pack_into('<II', out, 4, 1, 63)
            struct.pack_into('<10Q', out, 16, 4, 0x3c0, 0, 0, 0, 0, 0x40160000, 0, 123456789, 24000000)
            return bytes(out)
        if sub == 1:
            address, write = struct.unpack('<QI', body)
            return struct.pack('<Q', address & ~0xfff)  # bit0 clear: mapped
        if sub == 2:
            address, count = struct.unpack('<QI', body)
            return self.get(address, count)
        if sub == 3:
            address, count = struct.unpack_from('<QI', body)
            self.put(address, body[12:12 + count])
            return struct.pack('<I', count)
        if sub == 4:
            out = bytearray(64)
            out[:4] = b'FXS1'
            struct.pack_into('<4I', out, 4, 1, 3, self.phase, self.job)
            struct.pack_into('<QIIQ4I', out, 24, getattr(self, 'result', 0), len(self.output), 0,
                             0, 0, 0, 0, getattr(self, 'kind', 0))
            return bytes(out)
        if sub == 5:
            kind, timeout = struct.unpack_from('<II', body)
            if self.phase != 0:
                raise ValueError('busy')
            self.job += 1
            self.kind = kind
            if kind == 1:
                address, *call_args = struct.unpack('<9Q', body[8:])
                self.lines.append(f'call {address:#x} {[hex(a) for a in call_args[:2]]}')
                self.output = b''
                self.result = self.call(address, call_args)
            else:
                self.result = 0
                self.output = self.run(body[8:].decode())
            self.phase = 3
            return struct.pack('<I', self.job)
        if sub == 6:
            job, at, count = struct.unpack('<III', body)
            return self.output[at:at + count]
        if sub == 7:
            self.phase = 0
            return b''
        raise ValueError('sub')

    def call(self, address, args):
        if address == 0x4104d2dc:  # pool allocation -> descriptor
            self.put(0xa60000000 + 8, struct.pack('<Q', 0xa60000100))
            self.put(0xa60000100, struct.pack('<QI', 0x8a83fb000, 0x2000000))
            return 0xa60000000
        if address == 0x402f2d58:  # Wave5 bring-up
            self.put(0xa02d3d928 + 0x58, struct.pack('<Q', 0x16010000)); return 0
        if address == 0x402f3084:
            self.put(0xa02d3d928 + 0x58, bytes(8)); return 0
        if address == 0x40d2c408:  # heap allocation
            return 0xa50000000
        files = {'A:\\CODEC00.TXT': b'ADDRESS VALUE\r\n' + b'0x0000  0x009F\r\n' * 9000}
        def cstr(at):
            out = b''
            while (c := self.get(at + len(out), 1)) != b'\0':
                out += c
            return out.decode()
        if address == 0x402ddc6c:  # open_dir(path, &fd)
            self.dir_left = ['DCIM', 'NIKON', 'CODEC00.TXT', 'TEST0009.WAV']
            self.put(args[1], struct.pack('<i', 7)); return 0
        if address == 0x402ddd1c:  # read_dir(fd, entry)
            if not self.dir_left:
                return 0x80002005
            name = self.dir_left.pop(0).encode()
            self.put(args[1], bytes(0x10) + name + b'\0'); return 0
        if address in (0x402ddd6c, 0x402dd558):
            return 0
        if address == 0x402dd4f8:  # open(path, 0, 0)
            path = cstr(args[0])
            if path not in files:
                return 0xfffffffe
            self.file, self.file_at = files[path], 0; return 3
        if address == 0x402dd6c8:  # read(fd, buf, n, &done)
            chunk = self.file[self.file_at:self.file_at + args[2]]
            self.put(args[1], chunk); self.put(args[3], struct.pack('<I', len(chunk)))
            self.file_at += len(chunk); return 0
        if address == 0x40c80988:  # message post
            msg_id = struct.unpack('<I', self.get(args[0] + 0x20, 4))[0]
            payload = struct.unpack('<Q', self.get(args[0] + 0x28, 8))[0]
            self.posted.append((msg_id, payload))
            if msg_id == 0x42040106:
                self.put(STATE, struct.pack('<I', 5)); self.recording = True
            if msg_id == 0x42040108:
                self.put(STATE, struct.pack('<I', 1)); self.recording = False
            return 1
        return 0

    def run(self, line):
        self.lines.append(line)
        argv = line.split()
        if argv[0] == 'echo':
            return (' '.join(argv[1:]) + '\n').encode()
        if argv[0] == 'help':
            names = ['echo', 'help', 'memdump'] + (['audiodbg'] if self.help_lists_audiodbg else [])
            return ('command list:\n' + '\n'.join(names) + '\n').encode()
        if argv[0] != 'audiodbg':
            return b'command not found\n'
        code, a = int(argv[1]), [int(x) for x in argv[2:]] + [0, 0, 0]
        if code == 21:
            self.put(DEPTH_ARG, struct.pack('<I', a[0]))
        elif code == 1 and a[0] == 1 and a[1] <= 3:
            self.put(DEPTH_LATCHED, self.get(DEPTH_ARG, 4))
            self.put(BUFFER_PTR, struct.pack('<Q', 0xa20000000))
            self.put(STATE, struct.pack('<I', 2))
            self.recording = True
        elif code == 1 and a[0] == 0:
            self.put(STATE, struct.pack('<I', 1))
            self.recording = False
        elif code == 20:
            self.put(FILE_NO, struct.pack('<H', a[0]))
        return b''
