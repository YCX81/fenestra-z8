#!/usr/bin/env python3
"""summary.py RUN...: per-checkpoint view of the fields that decide the v37 scenario."""
import json, sys
from pathlib import Path

ROOT = Path.home() / '.local/share/nikon-z8-qemu/v37-20261111'
for name in sys.argv[1:]:
    path = ROOT / name / 'live-checkpoints.jsonl'
    log = (ROOT / (name + '.log')).read_text(errors='replace') if (ROOT / (name + '.log')).exists() else ''
    print('==', name, 'finished' if 'Finished' in log else 'running', 'TRACEBACK' if 'Traceback' in log else '')
    if not path.exists():
        continue
    for line in path.read_text().splitlines():
        r = json.loads(line)
        label = r['label']
        if not any(k in label for k in ('icon', 'pb-', 'post', 'photo-live', 'final', 'press', 'pose', 'movie')) or label.endswith('-release'):
            continue
        live, ui, pb = r.get('live', {}), r.get('ui', {}), r.get('pb', {})
        print('%-26s view=%s/%s mode=%s | live phase=%s job=%s enc=%s tx=%s err=%s irq=%s | icon att=%s pres=%s fail=%s heal=%s | pb valid=%s play=%s frames=%s rend=%s clip=%s dec_err=%s blocked=%s att=%s rec=%s lookup=%s | audio svc=%s drv=%s stage=%s plays=%s err=%s' % (
            label.split('-', 2)[-1] if label.startswith('step-') else label, r.get('view_id'), r.get('view_state'), r.get('photo_movie'),
            live.get('capture.phase'), live.get('stream.job'), live.get('stream.encoded'), live.get('transaction_count'),
            live.get('transaction_error'), live.get('capture.irq_seq'),
            ui.get('display.attached'), ui.get('presents'), ui.get('present_fail'), ui.get('icon_heals'),
            pb.get('cache_valid'), pb.get('playing'), pb.get('frames'), pb.get('rendered_frames'), pb.get('clip_mode'),
            pb.get('decode_errors'), pb.get('blocked_key_epoch'), pb.get('display.display.attached'), pb.get('consumed_rec'),
            pb.get('lookup_error'), r.get('audio_service'), r.get('audio_driver'), pb.get('audio_stage'),
            pb.get('audio_plays'), pb.get('audio_errors')))
    files = ROOT / name / 'files'
    if files.exists():
        for f in sorted(files.rglob('*')):
            if f.is_file():
                print('   card:', f.relative_to(files), f.stat().st_size)
