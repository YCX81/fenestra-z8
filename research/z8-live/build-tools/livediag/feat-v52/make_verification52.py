"""Assemble the release record only after exact-image and close/reopen checks pass."""
from pathlib import Path
import hashlib, json

root = Path(__file__).resolve().parent
evidence = root/'results-g'
read = lambda p: json.loads(p.read_text())
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
core = read(evidence/'core-validation.json')
close = read(evidence/'close-core-validation.json')
reopen = read(evidence/'reopen-validation.json')
linked = read(evidence/'linked-audio-handoff.json')
native = read(root/'native-playback-g/evidence/summary.json')
audit_path = root/'portrait-rec/review/final-binary-g.json'
audit = read(audit_path)
owner = read(root/'portrait-rec/review/final-binary-owner-g.json')
ready = read(root/'portrait-rec/review/final-binary-ready-g.json')
sources = read(root/'integration/source-g.json')
main = audit['main_sha256']
assert core['passed'] and close['passed'] and reopen['passed'] and linked['passed']
assert audit['result'] == native['result'] == 'PASS'
assert core['main_sha256'] == linked['main_sha256'] == native['executed_main_sha256'] == main
assert close['main_sha256'] == reopen['diagnostic_main_sha256']
assert sources['src-fix37/codec/live_w5enc.c'] == reopen['codec_source_sha256'] == owner['source_sha256'] == ready['source_sha256']
assert owner['pass'] and ready['passed']
assert sha(root/'integration/source-g.json') == audit['source_manifest_sha256']
movie = dict(core['movie'], name='DSC_9051.MOV',
             description='Actual final-G QEMU Live capture, silent because emulator microphone supplies no PCM; timing/video test only.')
v = dict(release_status='qemu_test_candidate', version='v52',
         main_path='/path/to/user/fxfix/v52-root/v52g/q/main.main', main_sha256=main,
         build_id=audit['build_id'], expected_package_sha256=audit['package_sha256'],
         core_regression_passed=True, close_reopen_regression_passed=True,
         binary_audit_passed=True, physical_camera_verified=False,
         core=core, diagnostic_close_core=close, diagnostic_close_reopen=reopen,
         native_playback=native, linked_audio_handoff=linked,
         binary_audit_sha256=sha(audit_path), source_manifest_sha256=audit['source_manifest_sha256'],
         source_files_checked=audit['immutable_source_files_checked'],
         linked_arm_checks=dict(owner=owner['cases'], ready=ready['count'],
                                elf_sha256=owner['elf_sha256']),
         test_movies=[movie],
         nonpassing_diagnostic_runs=[dict(
             run='v52g-hevc-close',
             error='No verified Live photo owner for REC',
             observation='First LV IRQ stopped at 464 after menu exit; requested=1, hold=0, hw=0, VDI=0. No SET_PARAMS/HEADERS call occurred.',
             handling='Original failure retained; close/reopen acceptance uses a separate unchanged-image/driver repeat, recorded above.')],
         limitations=[
             'No physical camera acceptance for this image; Live On native-player resource takeover needs hardware verification.',
             'QEMU has no real microphone or speaker. Gain/noise, complete REC audibility and synchronization require new camera recordings.',
             'Private PB audio protocol test used zero frames and a prior C build: open/buffer/start/stop callbacks passed; that C full recovery failed. Not an audible or DMA-throughput test.',
             'Shutter audibility is not repaired; passive full-press/prepare/fire/mute diagnostics are included. No forced shutter-sound behavior.',
             'EVF portrait layout/icon/REC output patches passed record and ARM checks; physical LCD/EVF appearance still needs verification.',
             'New Live files use fixed 30000/1001 timing. Existing 9044 remains unchanged.',
             'Native 1024x682 QEMU playback has an unresolved display-model MMIO boundary; full file decoding was separately checked with ffmpeg.',
             'Generic partial initialization failures retain native-owned pools rather than unsafe free/retry; no general partial-open abort implementation is claimed.',
         ],
         preserved=dict(v49_original_fonts=True, preparing_text=True,
                        portrait_records=audit['portrait_records'], evf_core_bytes=audit['evf_core_bytes'],
                        size_meta_gap=audit['size_meta_gap_preserved'],
                        plugin_sha256=audit['plugin_regular_files']))
(evidence/'verification.json').write_text(json.dumps(v, ensure_ascii=False, indent=2)+'\n')
print(json.dumps(dict(main_sha256=main, expected_package_sha256=v['expected_package_sha256'],
                     test_movie_sha256=movie['sha256']), indent=2))
