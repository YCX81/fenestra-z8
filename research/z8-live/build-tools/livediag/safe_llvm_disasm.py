#!/usr/bin/env python3
"""Safe ctypes binding to Apple's LLVM disassembler; no third-party packages.

Example:
  python3 safe_llvm_disasm.py bg34.main --base 0x40160000 \
      --start 0x40c06fe0 --length 0x140
Use Disassembler as a context manager when importing this module.
"""
import argparse
import ctypes as C
from pathlib import Path
import threading

LIBRARIES = (
    '/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/lib/libLTO.dylib',
    '/Library/Developer/CommandLineTools/usr/lib/libLTO.dylib',
)
_INIT_LOCK = threading.Lock()

class Disassembler:
    def __init__(self, library=None, triple='aarch64-none-none'):
        self._context = None
        self._lock = threading.Lock()
        if library is None:
            library = next((p for p in LIBRARIES if Path(p).is_file()), None)
        if library is None:
            raise FileNotFoundError('No Apple libLTO.dylib found; install Xcode Command Line Tools')
        self.library = str(library)
        self._lib = lib = C.CDLL(self.library)
        # Declare every C signature, especially pointer returns and size_t.
        lib.lto_initialize_disassembler.argtypes = []
        lib.lto_initialize_disassembler.restype = None
        lib.LLVMCreateDisasm.argtypes = [C.c_char_p, C.c_void_p, C.c_int, C.c_void_p, C.c_void_p]
        lib.LLVMCreateDisasm.restype = C.c_void_p
        lib.LLVMDisasmInstruction.argtypes = [C.c_void_p, C.POINTER(C.c_uint8), C.c_uint64,
                                             C.c_uint64, C.POINTER(C.c_char), C.c_size_t]
        lib.LLVMDisasmInstruction.restype = C.c_size_t
        lib.LLVMDisasmDispose.argtypes = [C.c_void_p]
        lib.LLVMDisasmDispose.restype = None
        encoded = triple.encode('ascii')
        if b'\0' in encoded:
            raise ValueError('Triple must not contain NUL')
        with _INIT_LOCK:
            # libLTO does not register its target disassemblers merely on load.
            lib.lto_initialize_disassembler()
            self._context = lib.LLVMCreateDisasm(encoded, None, 0, None, None)
        if not self._context:
            raise ValueError(f'LLVM could not create a disassembler for {triple!r}')

    def instruction(self, data, address=0):
        if not isinstance(data, (bytes, bytearray, memoryview)):
            raise TypeError('data must be bytes-like')
        data = bytes(data)
        if not isinstance(address, int) or not 0 <= address <= (1 << 64) - 1:
            raise ValueError('address must fit uint64')
        with self._lock:
            if not self._context:
                raise RuntimeError('Disassembler is closed')
            if not data:
                return 0, ''
            buf = (C.c_uint8 * len(data)).from_buffer_copy(data)
            out = C.create_string_buffer(4096)
            size = self._lib.LLVMDisasmInstruction(self._context, buf, len(data), address, out, len(out))
            if size > len(data):
                raise RuntimeError('LLVM returned an invalid instruction length')
            return size, out.value.decode('utf-8', errors='replace').strip()

    def close(self):
        with self._lock:
            if self._context:
                self._lib.LLVMDisasmDispose(self._context)
                self._context = None

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


def main():
    parser = argparse.ArgumentParser(description='Read-only AArch64 disassembly of a flat binary')
    parser.add_argument('binary', type=Path)
    number = lambda x: int(x, 0)
    parser.add_argument('--base', type=number, required=True)
    parser.add_argument('--start', type=number, required=True)
    parser.add_argument('--length', type=number, required=True)
    parser.add_argument('--library', choices=LIBRARIES)
    args = parser.parse_args()
    offset = args.start - args.base
    if (not 0 <= args.base <= args.start < 1 << 64 or args.start % 4
            or args.length <= 0 or args.length % 4 or args.start + args.length > 1 << 64
            or offset + args.length > args.binary.stat().st_size):
        parser.error('Range must be aligned, positive, within the binary and uint64 address space')
    with args.binary.open('rb') as stream:
        stream.seek(offset)
        data = stream.read(args.length)
    if len(data) != args.length:
        parser.error('Binary changed or range could not be read completely')
    with Disassembler(args.library) as dis:
        for offset in range(0, len(data), 4):
            chunk = data[offset:offset+4]
            size, text = dis.instruction(chunk, args.start + offset)
            if size != 4:
                text = f'.word 0x{int.from_bytes(chunk, "little"):08x}'
            print(f'{args.start+offset:#x}  {text}')

if __name__ == '__main__':
    main()
