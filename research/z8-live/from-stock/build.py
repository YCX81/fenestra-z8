"""Reconstruct v36 from original C 3.11 and public source/dependencies on Linux."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import urllib.request
import zipfile

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
ARCHIVE = HERE.parent
RELEASE = ARCHIVE / 'build-tools/livediag/feat-release'
LOAD = 0x40160000
STOCK = 'd67413c2ede6b9ce8887175f20700bd9fbad2a973a4820968c45503d15d80d96'
CORE = 'bc01ea90f042e036d2894879156c33f80873e8ad472295ebfe89dc9275c642a6'
CORE_BASE = 'c770f34014849b2c42f20b9258a32b8959253cfb296e48f83417572781e00864'
RUNTIME_ID = 'a45bb382924140bcc2c3655e777ae1138927c51a09f25ffdef618fb5766b7577'
PINS = {
    'nfp-r1': '8bc079ddb367aceb11d0f65e59bef39081c27b34bc160ee6cf08a80718b31f56',
    'nfp-rc2': '71c1475f2dd2f87a9d3f10f90408f9aa0487d62fbfa3dfb2ceb8a0b05140e382',
    'seed': '50f2a5b22217710c6f35faf1e4639ef675f2738184525be0190f36bcc42cb36b',
    'playback': 'a46b334d28b72f8f9899f11731c3537a607144300d399a9d6dac82d7629bced2',
    'usb': '56395e20d394f9f454a4160497df07e2c2e40e97433d82f522378c6ef70b897a',
    'pool': '98638cf72deb1980fb7b3cdf2b82501f112303dad0ba9beb9a6757517b790544',
    'main': 'fecbe07e3a0ea2a9136b8e6a396e06b81d3d4e81eda7881c4b98ae20653be1f4',
    'package': 'a6e1d92ef0c52fc4af066eb128460f8c01415e282ade12070e1c4bf5d70a6cde',
}
DEPENDENCIES = {
    'r1': ('v0.3.0-r1', 1022549,
           '704dbb2e09abed4e641174810ad655a413a2eff4042845371499c896634591ed'),
    'rc2': ('v0.4.0-rc.2', 8046605,
            '9a00183b54ee80e02e89445a59f5806a1e738ea22e2400538221ca27c21d9b4c'),
}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def file_sha(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def compiler(value, major):
    resolved = shutil.which(value)
    if not resolved:
        raise ValueError('missing compiler: ' + value)
    resolved = str(Path(resolved).resolve())
    version = subprocess.check_output([resolved, '--version'], text=True)
    if not re.search(r'clang version ' + str(major) + r'\.', version):
        raise ValueError('requires clang ' + str(major) + ': ' + version.splitlines()[0])
    return resolved, version


def toolkit(kind, supplied, out):
    tag, size, digest = DEPENDENCIES[kind]
    bundled = HERE / 'third-party' / ('NikonFirmwarePatch-' + tag + '-toolkit.zip')
    if supplied:
        archive = supplied.resolve()
    elif bundled.is_file():
        archive = bundled
    else:
        archive = out / (kind + '.zip')
        url = ('https://github.com/XinmiaoZhao/NikonFirmwarePatch/releases/download/'
               + tag + '/NikonFirmwarePatch-' + tag + '-toolkit.zip')
        request = urllib.request.Request(url, headers={'User-Agent': 'v36-reproduction'})
        with urllib.request.urlopen(request, timeout=60) as response, archive.open('xb') as target:
            remaining = size
            while remaining:
                block = response.read(min(1024 * 1024, remaining))
                if not block:
                    break
                target.write(block)
                remaining -= len(block)
            if remaining or response.read(1):
                raise ValueError('public toolkit length differs: ' + kind)
    if archive.stat().st_size != size or file_sha(archive) != digest:
        raise ValueError('public toolkit SHA-256 differs: ' + kind)
    destination = out / kind
    destination.mkdir()
    with zipfile.ZipFile(archive) as package:
        if len(package.infolist()) > 2000 or sum(x.file_size for x in package.infolist()) > 100 * 1024 * 1024:
            raise ValueError('unexpected toolkit archive inventory')
        for entry in package.infolist():
            candidate = (destination / entry.filename).resolve()
            if not candidate.is_relative_to(destination.resolve()):
                raise ValueError('toolkit path escapes destination')
        package.extractall(destination)
    return destination / ('NikonFirmwarePatch-' + tag)


def combine(stock, image, layer, label):
    for at, (before, after) in enumerate(zip(stock, layer)):
        if before == after:
            continue
        if image[at] not in (before, after):
            raise ValueError(label + ' overlaps another layer at ' + hex(LOAD + at))
        image[at] = after


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stock', type=Path, required=True)
    parser.add_argument('--qemu-root', type=Path, default=REPO.parent / 'z8-qemu')
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--core-clang', default='clang-18')
    parser.add_argument('--live-clang', default='clang-21')
    parser.add_argument('--llvm-bin', type=Path, help='directory containing LLVM 21 linker/object tools')
    parser.add_argument('--nfp-r1-toolkit', type=Path)
    parser.add_argument('--nfp-rc2-toolkit', type=Path)
    parser.add_argument('--main-only', action='store_true', help='skip the final update-container reconstruction')
    args = parser.parse_args()
    if os.name != 'posix':
        parser.error('run inside Linux or WSL')
    original = args.stock.resolve()
    if original.stat().st_size != 99051117 or file_sha(original) != STOCK:
        parser.error('provide the complete original Z_8_0311.bin')
    core_clang, core_version = compiler(args.core_clang, 18)
    live_clang, live_version = compiler(args.live_clang, 21)
    llvm_bin = args.llvm_bin or Path(live_clang).parent
    os.environ['PATH'] = str(llvm_bin.resolve()) + os.pathsep + os.environ['PATH']
    for tool in ('ld.lld', 'llvm-nm', 'llvm-objcopy', 'llvm-objdump'):
        if not shutil.which(tool):
            parser.error('missing LLVM 21 tool: ' + tool)
    linker = subprocess.check_output(['ld.lld', '--version'], text=True)
    if not re.search(r'LLD 21\.', linker):
        parser.error('use LLVM 21 object/link tools via --llvm-bin')
    qemu = args.qemu_root.resolve()
    helpers = qemu / 'experiments/20261111/v36/from-stock'
    if not (helpers / 'z8_video_layout.py').is_file():
        parser.error('update the companion z8-qemu main branch and pass --qemu-root')
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=False)
    logs = out / 'logs'
    logs.mkdir()
    dependencies = out / 'dependencies'
    dependencies.mkdir()
    receipt = dict(schema='v36.from-stock-reconstruction/v1',
                   original_package_sha256=STOCK, core_compiler=core_version,
                   live_compiler=live_version, private_intermediate_inputs=[],
                   public_toolkits={k: dict(tag=v[0], sha256=v[2]) for k, v in DEPENDENCIES.items()},
                   stages=[], physical_camera_verified=False, qemu_rerun=False)

    def save():
        (out / 'reconstruction.json').write_text(json.dumps(receipt, indent=2) + '\n')

    def run(name, command, *, cwd=None, environment=None):
        with (logs / (name + '.log')).open('w') as log:
            result = subprocess.run(list(map(str, command)), cwd=cwd,
                                    env=environment, stdout=log, stderr=subprocess.STDOUT)
        if result.returncode:
            raise RuntimeError(name + ' failed; inspect ' + str(logs / (name + '.log')))

    def record(name, path, expected=None):
        digest = file_sha(path)
        receipt['stages'].append(dict(stage=name, file=str(path.relative_to(out)),
                                     bytes=path.stat().st_size, sha256=digest))
        save()
        if expected and digest != expected:
            raise ValueError(name + ' differs from recorded v36: ' + digest)
        print(name + ': ' + digest, flush=True)

    packages = {k: toolkit(k, getattr(args, 'nfp_' + k + '_toolkit'), dependencies)
                for k in DEPENDENCIES}
    backend = HERE / 'nfp_backend.py'
    stock_path = out / 'stock.main'
    run('decode', [sys.executable, '-B', backend, 'decode', '--toolkit', packages['rc2'],
                   '--stock', original, '--output', stock_path])
    stock = stock_path.read_bytes()
    for kind in ('r1', 'rc2'):
        run('nfp-' + kind, [sys.executable, '-B', backend, kind, '--toolkit', packages[kind],
                           '--stock', original, '--output', out / ('nfp-' + kind + '.main')])
        record('nfp-' + kind, out / ('nfp-' + kind + '.main'), PINS['nfp-' + kind])
    core = out / 'work/core'
    core.mkdir(parents=True)
    for folder in ('host', 'emulation'):
        shutil.copytree(REPO / folder, core / folder, ignore=shutil.ignore_patterns('__pycache__'))
    for folder in ('include', 'src'):
        shutil.copytree(ARCHIVE / 'baseline-core' / folder, core / folder)
    shutil.copytree(HERE / 'core-overlay', core, dirs_exist_ok=True)
    core_build = core / 'build/core'
    environment = dict(os.environ, V36_CORE_CLANG=core_clang)
    run('core', [sys.executable, '-B', core / 'host/build_z8_core.py', '--firmware', stock_path,
                 '--output-dir', core_build], cwd=core, environment=environment)
    metadata = json.loads((core_build / 'build.json').read_text())
    candidate = bytearray((core_build / 'z8-core.main').read_bytes())
    identity_at = metadata['symbols']['fx_z8_debug_build'] - LOAD
    if candidate[identity_at:identity_at + 32].hex() != metadata['debug_build_identity']:
        raise ValueError('generated core identity preimage differs')
    receipt['current_core_source_identity'] = metadata['debug_build_identity']
    receipt['historical_runtime_identity'] = RUNTIME_ID
    candidate[identity_at:identity_at + 32] = bytes.fromhex(RUNTIME_ID)
    if metadata['code_bytes'] != 268776 or sha(candidate[0x40000:0x40000 + metadata['code_bytes']]) != CORE:
        raise ValueError('compiled historical core differs beyond its normalized identity')
    sys.path[:0] = [str(helpers), str(qemu / 'tools')]
    from z8_slots_image import compose, overlay
    from z8_slots_layout import Layout
    from z8_orientation_unlock import build_plan
    image = bytearray((out / 'nfp-r1.main').read_bytes())
    combine(stock, image, candidate, 'core')
    orientation = build_plan(stock, video=True, video_portrait=True, video_file_rotation=True)['image']
    combine(stock, image, orientation, 'portrait')
    image, _ = overlay(bytes(image), compose(stock, Layout(64)))
    image = bytearray(image)
    movie = out / 'work/movie'
    movie.mkdir()
    source = (HERE / 'legacy/movie_hook.c').read_text()
    for before, after in [('0x408c0024', '0x40a02024'), ('0xa0fec3c00', '0xa10005c00'),
                          ('0xa0fec3c80', '0xa10005c80'), ('0xa0fec4000', '0xa10006000')]:
        if before not in source:
            raise ValueError('legacy movie relocation source differs')
        source = source.replace(before, after)
    (movie / 'hook.c').write_text(source)
    run('movie-compile', [core_clang, '--target=aarch64-none-elf', '-ffreestanding', '-fno-builtin',
                          '-fno-stack-protector', '-mgeneral-regs-only', '-fno-pic', '-O1',
                          '-c', movie / 'hook.c', '-o', movie / 'hook.o'])
    run('movie-link', ['ld.lld', '-T', HERE / 'legacy/movie_hook.ld', '-e', 'live_entry',
                       movie / 'hook.o', '-o', movie / 'hook.elf'])
    run('movie-binary', ['llvm-objcopy', '-O', 'binary', movie / 'hook.elf', movie / 'hook.bin'])
    text = subprocess.check_output(['llvm-nm', '--defined-only', str(movie / 'hook.elf')], text=True)
    symbols = {p[2]: int(p[0], 16) for line in text.splitlines() if len(p := line.split()) == 3}
    code = (movie / 'hook.bin').read_bytes()
    if sha(code) != '55b7ab9ef88d0a9dbadb033bb5c21fee7a098d534af665c98585e9fa15c6601b':
        raise ValueError('legacy movie source did not reproduce its payload')
    at = 0x40a02000 - LOAD
    if any(image[at:at + len(code)]):
        raise ValueError('legacy movie payload overlaps another layer')
    image[at:at + len(code)] = code
    for address, name, call in [(0x4034974c, 'live_entry', True), (0x40fdc9c8, 'live_press_entry', False),
                                (0x40c59d18, 'live_picture_entry', True), (0x40d09710, 'live_take_entry', True),
                                (0x40d09764, 'live_batch_entry', True)]:
        at = address - LOAD
        if image[at:at + 4] != stock[at:at + 4]:
            raise ValueError('legacy movie hook overlaps another layer')
        delta = symbols[name] - address
        if delta % 4 or not -(1 << 27) <= delta < (1 << 27):
            raise ValueError('legacy movie branch out of range')
        struct.pack_into('<I', image, at, (0x94000000 if call else 0x14000000) | ((delta // 4) & 0x3ffffff))
    baseline = out / 'core-baseline.main'
    baseline.write_bytes(image)
    record('core-baseline', baseline, CORE_BASE)
    # Replace the old public NFP layer while retaining the larger C64 copy extent.
    old, new = (out / 'nfp-r1.main').read_bytes(), (out / 'nfp-rc2.main').read_bytes()
    copy_at = 0x41184d8c - LOAD
    copy_end = bytes(image[copy_at:copy_at + 8])
    def copied_extent(raw):
        low, high = struct.unpack('<II', raw)
        if low & 0xffe0001f != 0xd2800008 or high & 0xffe0001f != 0xf2a00008:
            raise ValueError('unexpected source copy-boundary instructions')
        return ((low >> 5) & 0xffff) | (((high >> 5) & 0xffff) << 16)
    if copied_extent(copy_end) < copied_extent(new[copy_at:copy_at + 8]):
        raise ValueError('C64 copy boundary does not cover the new NFP layer')
    for at, (original_byte, old_byte) in enumerate(zip(stock, old)):
        if original_byte != old_byte and not copy_at <= at < copy_at + 8:
            if image[at] != old_byte:
                raise ValueError('old NFP layer overlaps another owner at ' + hex(LOAD + at))
            image[at] = original_byte
    for at, (original_byte, new_byte) in enumerate(zip(stock, new)):
        if original_byte != new_byte and not copy_at <= at < copy_at + 8:
            if image[at] not in (original_byte, new_byte):
                raise ValueError('new NFP layer overlaps another owner at ' + hex(LOAD + at))
            image[at] = new_byte
    image[copy_at:copy_at + 8] = copy_end
    seed = out / 'seed.main'
    seed.write_bytes(image)
    record('source-generated-seed', seed, PINS['seed'])
    hooks = [0x403db694, 0x4032e130, 0x40d60ec8, 0x40fdc9c8, 0x40e90838, 0x40c26804,
             0x40c26818, 0x40c26828, 0x40e8b328, 0x408b7c1c, 0x406cd6a0, 0x406cd75c,
             0x40ea3638, 0x406cd468, 0x41020844, 0x40d60ecc, 0x4085c424, 0x4085bcb0,
             0x401cce84, 0x401cd3ec, 0x4061a724, 0x40619de4, 0x40618be0, 0x40618b84]
    ranges = [dict(address=hex(a), size=0, purpose='empty payload reservation', expected='', replacement='')
              for a in (0x401e2000, 0x40196000)]
    ranges += [dict(address=hex(a), size=4, purpose='source-generated hook preimage',
                    expected=bytes(image[a - LOAD:a - LOAD + 4]).hex(),
                    replacement=bytes(image[a - LOAD:a - LOAD + 4]).hex()) for a in hooks]
    seed_manifest = out / 'seed-manifest.json'
    seed_manifest.write_text(json.dumps(dict(schema='v36.from-stock-seed/v1', symbols=metadata['symbols'],
                                            ranges=ranges, output_sha256=sha(image), flashable=False), indent=2) + '\n')
    source = out / 'work/live'
    for name in ('src-fix36', 'pb-fix36'):
        shutil.copytree(RELEASE / name, source / name,
                        ignore=shutil.ignore_patterns('__pycache__', '*.o', '*.log'))
    environment = dict(os.environ, V36_REAL_CLANG=live_clang,
                       PATH=str(RELEASE / 'tools-v36/compiler') + os.pathsep + os.environ['PATH'])
    run('live', [sys.executable, '-B', RELEASE / 'relive_w5.py', source / 'src-fix36',
                 seed, seed_manifest, out / 'live'], environment=environment)
    if (out / 'live/payload.bin').stat().st_size > 0x401fe000 - 0x401e2000:
        raise ValueError('Live payload overlaps the bootguard helper')
    run('playback', [sys.executable, '-B', ARCHIVE / 'build-tools/pb/relive_pb.py', source / 'pb-fix36',
                     out / 'live/live-fixed.emulation-only.main', out / 'live/changed-ranges.json', out / 'pb',
                     '--defs={"fx_glyph":"0x401f0000","fx_memmove":"0x401f0100"}'], environment=environment)
    playback = out / 'pb/live-fixed.emulation-only.main'
    record('playback', playback, PINS['playback'])
    (out / 'usb').mkdir()
    usb = out / 'usb/usb.main'
    run('usb', [sys.executable, '-B', ARCHIVE / 'build-tools/usb_default_on.py', playback, usb])
    spec = importlib.util.spec_from_file_location('v36_chain', RELEASE / 'tools-v36/chain.py')
    chain = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(chain)
    manifest = chain.append_changes(playback.read_bytes(), usb.read_bytes(),
                                    json.loads((playback.parent / 'changed-ranges.json').read_text()), 'USB default on')
    (usb.parent / 'changed-ranges.json').write_text(json.dumps(manifest, indent=2) + '\n')
    record('usb', usb, PINS['usb'])
    (out / 'pool').mkdir()
    pool = out / 'pool/pool.main'
    run('pool', [sys.executable, '-B', ARCHIVE / 'build-tools/gui_pool_fix.py', usb, pool])
    record('pool', pool, PINS['pool'])
    final = out / 'v36.main'
    run('bootguard', [sys.executable, '-B', RELEASE / 'tools-v36/bootguard_patch.py', pool, final],
        environment=environment)
    record('main', final, PINS['main'])
    if not args.main_only:
        package = out / 'Z_8_0406.bin'
        run('package', [sys.executable, '-B', backend, 'pack', '--toolkit', packages['rc2'], '--stock', original,
                        '--main', final, '--output', package])
        record('package', package, PINS['package'])
    receipt['passed'] = True
    save()
    print('v36 reconstruction passed', flush=True)


if __name__ == '__main__':
    main()
