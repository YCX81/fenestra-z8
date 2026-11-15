"""Collect exact-image evidence; refuse incomplete v53 candidate packaging."""
from pathlib import Path
import argparse
import hashlib
import json

ROOT = Path(__file__).resolve().parent


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read(path):
    return json.loads(path.read_text())


def require(condition, explanation):
    if not condition:
        raise ValueError(explanation)


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('core_summary', type=Path)
    parser.add_argument('independent_review', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--legacy-summary', type=Path, required=True)
    parser.add_argument('--main', type=Path, default=ROOT / 'v53g/q/main.main')
    parser.add_argument('--binary-audit', type=Path,
                        default=ROOT / 'portrait/final-g-review/binary-audit.json')
    parser.add_argument('--native-dir', type=Path, default=ROOT / 'native-g')
    parser.add_argument('--prior-g-failure', type=Path,
                        default=ROOT / 'audio/evidence/final-g/final-summary.json')
    args = parser.parse_args()
    core = read(args.core_summary)
    require(core.get('core_runner_error') is None, 'Core runner ended with an error')
    require(core['core_runner_checks']['total'] > 0 and
            core['core_runner_checks']['failed'] == [], 'Core checks incomplete or failed')
    gates = ('capture_audio_window_passed', 'shutter_valid_resource_lifecycle_passed',
             'rec_pcm_dma_passed', 'rec_video_sequence_passed',
             'natural_eof_passed', 'stop_cleanup_passed', 'returned_to_audio_ready')
    for gate in gates:
        require(core.get(gate) is True, 'Incomplete behavioral gate: ' + gate)
    require(core.get('all_required_passed') is True, 'Core acceptance incomplete')

    kernel = core['identities']['kernel']
    sha = kernel['sha256']
    require(kernel['verified_before_launch'] is True, 'Kernel identity was not checked at launch')
    require(digest(args.main) == sha, 'Local MAIN differs from tested kernel')
    legacy = read(args.legacy_summary)
    legacy_kernel = legacy['identities']['kernel']
    require(legacy_kernel['sha256'] == sha and
            legacy_kernel['verified_before_launch'] is True,
            'Legacy playback used an unverified or different kernel')
    for gate in ('rec_pcm_dma_passed', 'rec_video_sequence_passed',
                 'natural_eof_passed', 'stop_cleanup_passed', 'fixture_unchanged'):
        require(legacy.get(gate) is True, 'Incomplete legacy behavioral gate: ' + gate)
    independent = read(args.independent_review)
    require(independent.get('passed') is True and independent.get('main_sha256') == sha and
            independent.get('summary_sha256') == digest(args.core_summary) and
            independent.get('legacy_summary_sha256') == digest(args.legacy_summary),
            'Independent review does not accept this exact result')
    audit_path = args.binary_audit
    audit = read(audit_path)
    require(audit['result'] == 'PASS' and audit['main_sha256'] == sha and
            audit['package_roundtrip_in_memory'] is True and
            audit['unselected_container_bytes_and_crcs_verified'] is True,
            'Binary/container audit differs or failed')
    native = {}
    for label, filename, pictures in (('4k', 'run-4k-summary.json', 95),
                                      ('live1024', 'run-real-summary.json', 98)):
        row = read(args.native_dir / filename)
        require(row['main_sha256'] == sha and row['all_checks_passed'] is True and
                row['error'] is None and row['unknown_mmio'] == [] and
                row['owned_qemu_stopped'] is True and row['native_picture_calls'] == pictures,
                'Native playback evidence incomplete: ' + label)
        native[label] = row
    fixture = native['live1024']['media_fixture']
    require(fixture['media_atoms_byte_identical'] is True and
            fixture['source_bytes'] - fixture['output_bytes'] ==
            fixture['removed_preview_bytes'] == 1870467 and
            fixture['output_sha256'] ==
            '4c11eccf0d46b68669ade4108ba24be2bf61dd5e6f06a025bd7f7d3c89609996',
            'Native Live fixture differs from the audited preview-free MOV')
    prior = read(args.prior_g_failure)
    require(prior['identities']['kernel']['sha256'] == sha and
            prior['core_runner_error'] is not None and prior['all_required_passed'] is False,
            'Earlier same-image GUI failure evidence missing or overwritten')

    result = dict(
        version='v53', release_status='qemu_test_candidate',
        main_path=kernel['path'], main_sha256=sha,
        expected_package_sha256=audit['package_sha256'],
        core_regression_passed=True, binary_audit_passed=True,
        native_4k_passed=True, native_live1024_passed=True,
        legacy_rec_pcm_dma_passed=True, legacy_rec_video_sequence_passed=True,
        physical_camera_verified=False, evf_icon_physical_verified=False,
        unresolved_pre_rec_gui_freeze_observed=True,
        native_full1280_playback_verified=False, native_rec_production_bridge=False,
        live_source='1280x852 EVF input accepted; saved output remains1024x682',
        preview='45 embedded JPEG preview frames retained for production REC',
        config='Existing dual-slot Live preferences retained; no reset or config overwrite',
        core_summary_sha256=digest(args.core_summary), core_summary=core,
        legacy_summary_sha256=digest(args.legacy_summary), legacy_summary=legacy,
        prior_same_image_failure_sha256=digest(args.prior_g_failure),
        prior_same_image_failure=prior,
        independent_review=independent, binary_audit=audit, native_playback=native,
        scope='Private functional QEMU model with declared PCM/tone2 inputs and clock conversion; '
              'not physical microphone, speaker, shutter timbre, latency, or EVF visibility acceptance',
        retained_failures='Earlier no-resource/clock/stimulus failures and F fixed-three-second '
                          'REC truncation retained in source evidence. REC idle global-reset model '
                          'dispatch conflict, earlier same-G pre-REC GUI freeze, and full1280 '
                          'research remain open. Successful read-only-observed repeat does not '
                          'establish that the earlier GUI freeze was fixed.',
    )
    result.update({gate: core[gate] for gate in gates})
    with args.output.open('x') as out:
        json.dump(result, out, ensure_ascii=False, indent=2)
        out.write('\n')
    print(json.dumps(dict(output=str(args.output), main_sha256=sha,
                          release_status=result['release_status']), indent=2))


if __name__ == '__main__':
    main()
