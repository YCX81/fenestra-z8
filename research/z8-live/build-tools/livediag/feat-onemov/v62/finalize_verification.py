#!/usr/bin/env python3
"""Bind completed V62 validation artifacts to the exact packaged image."""
from pathlib import Path
import hashlib
import json

ROOT = Path(__file__).resolve().parent
MAIN_SHA = '36c564c9e785822ccf784816882d5055ee8e5ca0b796e1c4fb8e459dfc1ae4b5'
BASE_SHA = 'db0c847b333f0a6e07222f2207435e9bf340ce1d544704f6d801ec3ecc1088e5'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def read(relative):
    return json.loads((ROOT / relative).read_text())


def passed(d):
    assert d['passed'] is True
    if isinstance(d.get('checks'), dict):
        assert d['checks'] and all(v is True for v in d['checks'].values())


def main():
    image = (ROOT / 'final/main.main').read_bytes()
    baseline = (ROOT.parent / 'v61/final/main.main').read_bytes()
    assert sha(image) == MAIN_SHA and sha(baseline) == BASE_SHA
    start, end = 0x40196000 - 0x40160000, 0x4019f020 - 0x40160000
    assert image[start:end] == baseline[start:end]
    audit = read('final/binary-audit.json')
    passed(audit)
    assert audit['final_main_sha256'] == MAIN_SHA
    assert not audit['unexplained_difference_addresses']
    menu = read('menu/final-context-arm-results.json')
    passed(menu)
    assert menu['candidate_main_sha256'] == MAIN_SHA
    adapter = read('shutter/adapter-arm.json')
    passed(adapter)
    assert adapter['source_sha256'] == sha((ROOT / 'work/src-fix37/audio/live_shutter_z8.c').read_bytes())
    native = read('shutter/native-profiles.json')
    passed(native)
    assert native['main_sha256'] == BASE_SHA
    model = read('verification/merged-tx16-model-tests.json')
    assert model['test_result']['passed'] and model['exit_code'] == 0 and not model['sanitizer_findings']
    capture_paths = ['verification/capture-on-paired-r2.json',
                     'verification/capture-on-tone10-r3.json',
                     'verification/capture-off-r4.json']
    captures = [read(p) for p in capture_paths]
    for capture in captures:
        passed(capture)
        assert capture['main_sha256'] == MAIN_SHA
        assert capture['qemu_sha256'] == '34059c8c62b3f32896b3d5685a461cd4da5a427ae5b9d3ba4e0f04318461f418'
    assert captures[0]['expected_tones'] == [11, 12]
    assert captures[1]['expected_tones'] == [10] and captures[1]['tx_bytes'] == 28800
    assert captures[1]['tx_sha256'] == '1a0a4f9641500aba12c1ade65b80fd1ab0494e107f08baf267d2cbd0dfb3a123'
    assert captures[2]['sound'] == 'off' and captures[2]['tx_bytes'] == 0 and not captures[2]['jobs']
    playback_paths = ['playback/on-r2-reboot/summary.json', 'playback/off-r4-reboot/summary.json']
    playbacks = [read(p) for p in playback_paths]
    for playback, capture in zip(playbacks, [captures[0], captures[2]]):
        assert playback['native_decode_return_cleanup_passed'] is True
        assert playback['identity']['main_sha256'] == MAIN_SHA
        assert playback['identity']['card_template'] == capture['run'] + '/capture-card.raw'
        assert playback['frame_coverage']['movie_sha256'] == capture['media'][0]['sha256']
        assert playback['frame_coverage']['source_frames'] == capture['media'][0]['video_frames']
    assert playbacks[0]['frame_coverage']['passed'] is False
    assert playbacks[0]['frame_coverage']['missing_source_indices'] == [91]
    assert playbacks[1]['frame_coverage']['passed'] is True
    evidence_paths = [
        'final/main.json', 'final/binary-audit.json', 'final/BINARY-AUDIT.md',
        'menu/final-context-arm-results.json', 'menu/final-text-results.md',
        'shutter/adapter-arm.json', 'shutter/native-profiles.json', 'shutter/evidence.json',
        'verification/merged-tx16-model-tests.json', 'verification/merged-model-manifest.json',
        'verification/native-sfx-assets.json', 'verification/ipc-and-retries.json',
        'verification/RESULT.md', 'playback/RESULT.md',
    ] + capture_paths + playback_paths
    evidence = [{'name': p.replace('/', '--'), 'path': '../' + p,
                 'sha256': sha((ROOT / p).read_bytes())} for p in evidence_paths]
    summary = {
        'version': 'v62', 'main_sha256': MAIN_SHA, 'baseline_main_sha256': BASE_SHA,
        'identity': '04.06.p0', 'build_id': '0xfb84a7ed',
        'release_checks_passed': True, 'all_observations_passed': False,
        'release_check_scope': 'Menu text resolution; actual On/Off SFX and microphone recovery; valid saved MOV; native playback entry/return/cleanup for both cards; complete source-frame coverage for Off only. On final-frame observation remains a documented limitation.',
        'physical_camera_verified': False,
        'playback_bank_identical_to_v61': True,
        'prior_v61_user_feedback': 'User confirmed RAW-priority Live badge, playback and return on camera.',
        'menu_arm_assertions': menu['checks'], 'shutter_arm_cases': adapter['case_count'],
        'native_soundtask_cases': native['case_count'], 'private_model_cases': model['test_result']['cases'],
        'unexplained_binary_changes': 0,
        'captures': [{k: d[k] for k in ('run', 'sound', 'main_sha256', 'qemu_sha256',
                                       'expected_tones', 'tx_bytes', 'tx_sha256', 'checks')}
                     for d in captures],
        'reboot_playbacks': [{'source_capture': d['identity']['card_template'],
                             'decoded_picture_events': d['decoded_pictures'],
                             'native_decode_return_cleanup_passed': d['native_decode_return_cleanup_passed'],
                             'frame_coverage': d['frame_coverage'], 'checks': d['checks']}
                            for d in playbacks],
        'limits': [
            'Menu verification executes final ARM text logic, not camera pixels; state/UI/owner services use explicit stubs.',
            'QEMU validates actual original SFX PCM consumption and microphone recovery; physical speaker acoustics/timing remain unverified.',
            'Native Silent remains effective. Finite profiles are covered; loop/Bulb takeover was not expanded.',
            'Enabled sound shares hardware with microphone and introduces a silence gap; tone10 model result was 0.25675 seconds.',
            'Native playback used rebooted private snapshots from On-r2 and Off-r4 captures, not same-boot or physical dual-card testing.',
            'On-r2 produced 92 picture events including one repeated first frame; source frames 0..90 were output, while final source frame91 was not observed. Return/cleanup passed. Off-r4 output all 93 source frames with one extra first-frame decode. No claim of complete On-frame coverage.',
            'An earlier pre-capture MCU ACK assertion remains unexplained; valid retry retains handshake guards. See retry receipt.',
        ],
        'evidence': evidence,
    }
    (ROOT / 'final/verification.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'release_checks_passed': True, 'all_observations_passed': False,
                      'main_sha256': MAIN_SHA, 'evidence_files': len(evidence)}))


if __name__ == '__main__':
    main()
