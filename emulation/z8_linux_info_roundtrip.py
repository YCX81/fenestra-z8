"""Execute the linked C3.11 bridge and production Linux agent on one file-backed pool.

Run on Linux with unicorn installed and qemu-aarch64 on PATH. The sparse file
stands in for physical memory and /dev/oscom_memhelper. No camera is accessed.
"""
import argparse
import ctypes
import hashlib
import json
import mmap
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile
import time

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_LR, UC_ARM64_REG_SP, UC_ARM64_REG_X0

MAIN = 0x40160000
POOL = 0xA0F000000
POOL_BYTES = 0x7FF000
SCRATCH = 0x20000000
STOP = SCRATCH + 0xF000
STATE = SCRATCH + 0x10000
PTP = SCRATCH + 0x18000
READY = 0xA02E920C4
CALLBACK = 0xA02E93530
RECEIVE = 0x40C9DA40


def run(build, qemu, directory):
    report = json.loads((build/'build.json').read_text())
    image = (build/'z8-core.main').read_bytes()
    agent = build/'fenestra-agent-z8-info'
    assert hashlib.sha256(image).hexdigest() == report['candidate_sha256']
    assert hashlib.sha256(agent.read_bytes()).hexdigest() == report['linux_agent_sha256']
    symbols = report['symbols']
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(MAIN, (len(image)+4095) & ~4095)
    uc.mem_write(MAIN, image)
    uc.mem_map(SCRATCH, 0x40000)
    uc.mem_map(0xA02E90000, 0x10000)

    def substitute(address, value):
        def return_value(machine, pc, size, context):
            machine.reg_write(UC_ARM64_REG_X0, value)
            machine.reg_write(UC_ARM64_REG_PC, machine.reg_read(UC_ARM64_REG_LR))
        uc.hook_add(UC_HOOK_CODE, return_value, begin=address, end=address)

    substitute(symbols['fx_z8_menu_linux'], STATE)
    substitute(symbols['fx_z8_menu_ptp'], PTP)
    # Single-threaded RTOS fixture: substitute only OS mutex init/take/give.
    for address in (0x40D2C564, 0x40D2C674, 0x40D2C660):
        substitute(address, 0)

    def call(address, *args):
        uc.reg_write(UC_ARM64_REG_SP, SCRATCH + 0x40000)
        uc.reg_write(UC_ARM64_REG_LR, STOP)
        for index, value in enumerate(args):
            uc.reg_write(UC_ARM64_REG_X0 + index, value)
        uc.emu_start(address, STOP, count=1000000)
        assert uc.reg_read(UC_ARM64_REG_PC) == STOP, hex(uc.reg_read(UC_ARM64_REG_PC))
        return uc.reg_read(UC_ARM64_REG_X0)

    def request(op, payload=b'', expected=0):
        packet = struct.pack('<I', op) + payload
        uc.mem_write(SCRATCH, packet)
        uc.mem_write(SCRATCH+0x2000, bytes(4))
        code = call(symbols['fx_linux_request'], SCRATCH, len(packet), SCRATCH+0x1000, SCRATCH+0x2000)
        assert code == expected, (op, code, expected)
        length, = struct.unpack('<I', uc.mem_read(SCRATCH+0x2000, 4))
        return bytes(uc.mem_read(SCRATCH+0x1000, length)) if length else b''

    device = directory/'dev/oscom_memhelper'
    device.parent.mkdir()
    with device.open('w+b') as file:
        file.truncate(POOL+POOL_BYTES)
        with mmap.mmap(file.fileno(), POOL_BYTES, offset=POOL) as pages:
            ptr = ctypes.addressof(ctypes.c_char.from_buffer(pages))
            uc.mem_map_ptr(POOL, POOL_BYTES, 7, ptr)
            # Use the stock initializer and allocator. Reserve a non-page-sized
            # block first so that alignment of the bridge allocation is exercised.
            assert call(0x40C9E808, POOL, POOL_BYTES) == 1
            assert call(0x40C9E48C, 0x8040) == POOL
            uc.mem_write(READY, b'\x01')
            uc.mem_write(CALLBACK, struct.pack('<Q', RECEIVE))
            nonce = 0x80311001
            request(1, struct.pack('<Q', nonce))
            allocation, shared = struct.unpack('<QQ', uc.mem_read(STATE, 16))
            assert allocation < shared == (allocation+4095) & ~4095
            assert struct.unpack('<Q',uc.mem_read(CALLBACK,8))[0] != RECEIVE
            offset = shared-POOL
            assert pages[offset:offset+8] == b'FXL3'+struct.pack('<I',8192)
            # Inspect the actual launch command from the linked core. Model
            # only the mount table and temporary directory for this shell test.
            for stage in range(4):
                data = struct.pack('<II',stage,stage+1) + (b'A'*512 if stage==1 else b'')
                uc.mem_write(SCRATCH,data)
                length = call(symbols['bootstrap'],STATE,SCRATCH,len(data),SCRATCH+0x1000)
                assert 0 < length <= 1000
                script = bytes(uc.mem_read(SCRATCH+0x1000,length)).decode('ascii')
                subprocess.run(['/bin/sh','-n'],input=script,text=True,check=True)
            staging = directory/'staging'
            staging.mkdir()
            mounts = directory/'mounts'
            mounts.write_text('tmpfs /tmp tmpfs rw 0 0\n')
            launch = script.replace('/proc/mounts',shlex.quote(str(mounts))).replace(
                f'/tmp/fenestra-{nonce:016x}',shlex.quote(str(staging)))
            staged_agent = staging/'agent'
            staged_agent.write_text('#!/bin/sh\ntouch unauthorized\n')
            marker = f'FXB{nonce:016x}00000004='
            output = subprocess.check_output(['/bin/sh','-c',launch],text=True)
            assert output.strip()==marker+'1' and not staged_agent.stat().st_mode & 0o111
            assert not (staging/'unauthorized').exists()
            # QEMU's path prefix provides only a sparse backing file. The guest
            # runs its production open/mmap with the original nonzero offset.
            process = subprocess.Popen([qemu,'-L',str(directory),str(agent),f'{shared:x}',f'{nonce:x}','FXL3'])
            def wait_word(at, target):
                deadline = time.monotonic()+4
                while struct.unpack_from('<I',pages,offset+at)[0] != target:
                    assert process.poll() is None and time.monotonic()<deadline, (at, process.poll())
                    call(symbols['fx_linux_tick'],0,0,1)
                    time.sleep(.005)
            try:
                wait_word(4128,1)
                request(2,b'id',5)
                request(4,struct.pack('<I',2),5)
                request(4,struct.pack('<II',1,0),5)
                assert struct.unpack_from('<I',pages,offset+128)[0] == 0
                sequence, = struct.unpack('<I',request(4,struct.pack('<I',1)))
                assert sequence == 1
                wait_word(2048,sequence)
                result = request(5,struct.pack('<I',sequence))
                err,op,magic,version,pid,uid,gid,jobs,files,ring,mask = struct.unpack_from('<II4s8I',result)
                assert (err,op,magic,version,jobs,files,ring,mask)==(0,1,b'FXL1',1,0,0,0,1)
                assert pid == struct.unpack_from('<I',pages,offset+4104)[0] and pid>0
                # Bypass the RTOS filter to exercise the agent's own deny path.
                struct.pack_into('<II',pages,offset+132,4,2)
                struct.pack_into('<I',pages,offset+128,2)
                wait_word(2048,2)
                assert struct.unpack_from('<III',pages,offset+2052)==(8,13,2)
                request(6)
                assert process.wait(timeout=3)==0
                assert struct.unpack_from('<I',pages,offset+4128)[0]==2
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
            uc.mem_unmap(POOL,POOL_BYTES)
            return dict(candidate=report['candidate_sha256'],agent=report['linux_agent_sha256'],
                        allocation=hex(allocation),shared=hex(shared),bytes=8192,
                        sequence=sequence,reply_bytes=len(result),magic=magic.decode(),pid=pid,
                        operations=mask,raw_shell_rejected=True,extra_args_rejected=True,
                        agent_denied_exec=True,wrong_agent_hash_rejected=True,revoked=True)


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build',type=Path)
    parser.add_argument('--qemu',default='qemu-aarch64')
    args=parser.parse_args()
    with tempfile.TemporaryDirectory() as temp:
        print(json.dumps(run(args.build.resolve(),args.qemu,Path(temp)),indent=2))
