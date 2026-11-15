"""Read the two immutable builds; recompile two units/relink in a temp dir only."""
from pathlib import Path
import ast, hashlib, json, shutil, subprocess, tempfile

roots = [Path('/path/to/user/fxfix/onemov/v61-raw-peer-final-d9f5/build'),
         Path('/path/to/user/fxfix/onemov/v62-log-shutter-8ec4cb/build')]
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
objects = [{str(p.relative_to(root)): sha(p) for p in root.rglob('*.o')} for root in roots]
assert objects[0].keys() == objects[1].keys()
changed = [n for n in objects[0] if objects[0][n] != objects[1][n]]
assert sorted(changed) == ['live/objects/audio_live_shutter_z8.c.o', 'live/objects/hooks.S.o']
recipes = []
for name in ['relive_v37.py', 'relive_pb53.py', 'live/payload.ld', 'pb/payload.ld']:
    hashes = [sha(r/name) for r in roots]
    assert hashes[0] == hashes[1]
    recipes.append(dict(name=name, sha256=hashes[0], identical=True))
source_sets = [{str(p.relative_to(r/'work')): sha(p) for p in (r/'work').rglob('*') if p.is_file()} for r in roots]
assert source_sets[0].keys() == source_sets[1].keys()
source_changes = [n for n in source_sets[0] if source_sets[0][n] != source_sets[1][n]]
assert sorted(source_changes) == ['src-fix37/audio/live_shutter_z8.c', 'src-fix37/hooks.S']
input_record = json.loads((roots[1].parent/'source-input.json').read_text())
assert all(input_record[n] == h for n, h in source_sets[1].items())
clang = Path(shutil.which('clang-21')).resolve()
lld = clang.parent/'ld.lld'
flags = ['--target=aarch64-none-elf', '-Os', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
         '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-ffunction-sections', '-fdata-sections',
         '-mgeneral-regs-only', '-mstrict-align', '-DLA_WIDTH=1024', '-DLA_HEIGHT=680',
         '-DLA_RING_CAPACITY=96', '-DLA_SAMPLE_INTERVAL_US=25000', '-DLIVE_BITRATE=8000000',
         '-DLIVE_IDR_US=500000', '-DLIVE_ENCODE_SPEED=9', '-Wall', '-Wextra', '-Werror']
recompiled = []; relinked = []
with tempfile.TemporaryDirectory(prefix='v62-independent-audit-') as temporary:
    temp = Path(temporary)
    for index, root in enumerate(roots):
        src = root/'work/src-fix37'
        replacements = {}
        for name in ['hooks.S', 'audio/live_shutter_z8.c']:
            obj = temp/f'{index}-{name.replace("/", "_")}.o'
            command = [str(clang), *flags, '-I', str(src), '-I', str(src/'ui/core/include'),
                       '-I', str(src/'codec/freestanding'), '-c', str(src/name), '-o', str(obj)]
            subprocess.run(command, check=True, capture_output=True)
            original = root/'live/objects'/(name.replace('/', '_')+'.o')
            assert obj.read_bytes() == original.read_bytes(), name
            replacements[name] = obj
            recompiled.append(dict(build=str(root), source=name, sha256=sha(obj), exact_object_match=True))
        for kind, recipe in [('live', 'relive_v37.py'), ('pb', 'relive_pb53.py')]:
            tree = ast.parse((root/recipe).read_text())
            names = next(ast.literal_eval(n.value) for n in tree.body if isinstance(n, ast.Assign)
                         and any(isinstance(t, ast.Name) and t.id == 'names' for t in n.targets))
            inputs = [replacements[n] if kind == 'live' and n in replacements
                      else root/kind/'objects'/(n.replace('/', '_')+'.o') for n in names]
            out = temp/f'{index}-{kind}.elf'
            subprocess.run([str(lld), '-T', str(root/kind/'payload.ld'), *map(str, inputs), '-o', str(out)],
                           check=True, capture_output=True)
            assert out.read_bytes() == (root/kind/'payload.elf').read_bytes(), (root, kind)
            relinked.append(dict(build=str(root), kind=kind, sha256=sha(out), exact_elf_match=True))
print(json.dumps(dict(passed=True, compiler=subprocess.check_output([str(clang), '--version'], text=True).splitlines()[0],
    linker=subprocess.check_output([str(lld), '--version'], text=True).strip(), compile_flags=flags,
    recipes=recipes, source_counts=list(map(len, source_sets)), source_changes=source_changes,
    work_source_matches_uploaded_input=True, objects=[dict(name=n, old_sha256=objects[0][n],
    new_sha256=objects[1][n], identical=objects[0][n]==objects[1][n]) for n in sorted(objects[0])],
    recompiled=recompiled, relinked=relinked, existing_source_or_artifact_modified=False), indent=2))
