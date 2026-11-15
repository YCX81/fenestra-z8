"""Copy the verified v51 candidate and four exact test fixtures to this SD card.

Default invocation only prepares the local plan and backs up the existing BIN.
--install performs the authorized card writes, then rereads every destination.
"""
from pathlib import Path
import datetime
import hashlib
import json
import os
import shutil
import sys

BASE = Path(__file__).resolve().parent
OUT = BASE / 'sd-card-20261114'
CARD = Path('/Volumes/NIKON Z 8  ')
PACKAGE = Path('/path/to/Nikon/delivery/Z8-v51-Portrait-LiveMOV-aa6e94e0')
OLD_BIN = '59700d880193d6ed25ca021b491b8f14ddc42aa3b63fb7f720055948f6330892'
NEW_BIN = 'c30031834f34ee7c25646502e775293aed99ecfd6358fae09e398a02e1ccf280'

def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(4 * 1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()

def entry(source, destination, digest, purpose):
    assert source.is_file() and sha(source) == digest, source
    return dict(source=str(source), destination=destination, sha256=digest,
                bytes=source.stat().st_size, purpose=purpose)

def card_identity():
    assert CARD.is_mount() and CARD.stat().st_dev == 16777241, 'SD mount identity changed'
    assert (CARD / 'DCIM/108NCZ_8').is_dir()
    assert shutil.disk_usage(CARD).free > 500_000_000

if '--install' not in sys.argv:
    card_identity()
    OUT.mkdir(exist_ok=True)
    assert sha(CARD / 'Z_8_0406.bin') == OLD_BIN
    backup = OUT / 'backup-before-v51'
    backup.mkdir(exist_ok=True)
    for rel in ['Z_8_0406.bin', 'FENESTRA/STARTUP-Z8.0', 'FENESTRA/portrait.wlm',
                'FENESTRA/slotcard.wlm', 'FENESTRA/LIVE-Z8.0', 'FENESTRA/LIVE-Z8.1']:
        source, dest = CARD / rel, backup / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        if not dest.exists():
            with source.open('rb') as src, dest.open('xb') as dst:
                shutil.copyfileobj(src, dst)
        assert sha(source) == sha(dest), rel
    items = [entry(PACKAGE / 'Z_8_0406.bin', 'Z_8_0406.bin', NEW_BIN, 'v51 update image')]
    for rel in ['FENESTRA/STARTUP-Z8.0', 'FENESTRA/portrait.wlm', 'FENESTRA/slotcard.wlm']:
        items.append(entry(PACKAGE / rel, rel, sha(PACKAGE / rel), 'matching companion; retain if identical'))
    samples = [
        ('writer/parser/LIV_9013.MOV', 'DSC_9041.MOV', '6eff4682f31abdf1e2050c38a3808afa3a5c6af958d1da1efc20420a6cb36801', '4K reference; 95 frames, audio'),
        ('writer/parser/native-final-audio.MOV', 'DSC_9042.MOV', '2aa43b037738eba33005aa524e69ca35b8234bbffe01ebb16923a12843dd843a', 'final writer fixture with reference 4K video; 95 frames, audio'),
        ('writer/nctg-tests/final/host-sample.MOV', 'DSC_9043.MOV', '84d3f5d81d42c91973467e8a2f0b3a904338f5f7fd421b6dbca33b5e9960bb39', 'final C writer 1024x682 host fixture; 95 frames, audio'),
        ('audio/qemu-v51d/runs/v51d-final-live/files/LIVE/100NCZ_8/DSC_0001.MOV', 'DSC_9044.MOV', '5bbc309ce4363e4d0040a2de0add71ddafca0d386cdf3324b7c298979da006d7', 'actual final-firmware QEMU capture; 1024x682, 77 frames, intentionally silent'),
    ]
    for source, name, digest, purpose in samples:
        items.append(entry(BASE / source, 'DCIM/108NCZ_8/' + name, digest, purpose))
    archive = PACKAGE.with_suffix('.zip')
    items.append(entry(archive, 'V51TEST/' + archive.name,
                       '270c1098273b44ca97e4903e3fea65fd2e5191432d27bb95ed1bb5e534025676',
                       'complete verified firmware package archive'))
    readme = OUT / 'README.txt'
    readme.write_text('''Nikon Z8 v51 SD卡测试（2026-11-14）

固件：根目录 Z_8_0406.bin。完整包在 V51TEST 内。
固件 SHA-256：c30031834f34ee7c25646502e775293aed99ecfd6358fae09e398a02e1ccf280
配套 FENESTRA 三个文件已与包核对一致；保留卡上 Live 设置。
卡上原固件和五个 FENESTRA 文件已备份到 Mac：
/path/to/Nikon/build-tools/livediag/feat-v50/sd-card-20261114/backup-before-v51
已完成文件复制及离线/QEMU验证；本记录不代表已经安装固件或完成真机验证。

四个短片都在 DCIM/108NCZ_8，用原厂回放按钮进入相册、选择视频并播放：
9041 / DSC_9041.MOV：4K 对照片，95帧，有音轨。
9042 / DSC_9042.MOV：新writer容器+同一4K视频，95帧，有音轨；验证新容器/Apple元数据/音频。
9043 / DSC_9043.MOV：新writer原尺寸1024×682片，95帧，有音轨；验证尺寸与有声路径。
9044 / DSC_9044.MOV：v51在QEMU实际拍出的1024×682 Live，77帧，约3.30秒，故意无音轨；验证实际写出文件。
9042/9043为诊断样片，不能用它们判断真机麦克风音质或声画同步。9044没有声音是预期。
这些文件复制进DCIM是为了测试原厂播放器，不代表固件已自动把LIVE目录短片登记进原厂相册。

建议测试顺序
1. 安装本次v51固件后，先将Live设为Off，按9041→9042→9043→9044播放。
   每个记下：是否出现在列表、缩略图、能否开始/完整播完、画面是否错位、是否有声音。
   若列表里看不到，先把“回放文件夹”设为“全部”，并区分“未列出”和“无法播放”。
2. 将Live设为On，等预录恢复后，同样播放四片；每片结束后半按快门返回，确认取景恢复。
   如果Off能播、On才沙漏或无法恢复，记录片号和发生阶段，这更直接指向资源交接。
3. Live On下拍一张新的Live：确认照片写出，REC能预览、退出后能继续拍下一张。
   再录一段普通录像并回放，检查Live开启时原厂录像/播放器能否接手。
4. 用真机新拍片检查录音周期刺啦声；检查竖屏视频菜单/直方图、中文Log菜单、EVF拍Live。

结果反馈可写成：固件版本；Live Off/On；9041/9042/9043/9044各自的播放结果；半按返回能否恢复。
如有错误或卡住，记录最后动作和屏幕信息，保留相关照片/短片供下一轮分析。

已知边界
Live开麦时的快门声仍未修。原尺寸完整回放、真实音质/同步、竖屏完整UI以及真机资源让路待本轮验证。
QEMU已通过Live拍摄→REC预览→恢复；4K样片完整播放。原尺寸片在QEMU解出首帧后受显示模型阻塞。
卡内原照片、旧测试视频、LIVE/LIVEDATA、索引文件均保留；新短片编号9041～9044。
''', encoding='utf-8')
    items.append(entry(readme, 'V51TEST/README.txt', sha(readme), 'test order and limitations'))
    for item in items:
        dst = CARD / item['destination']
        if dst.exists():
            assert item['destination'] == 'Z_8_0406.bin' or sha(dst) == item['sha256'], dst
    before = {str(p.relative_to(CARD)): {'bytes': p.stat().st_size, 'mtime_ns': p.stat().st_mtime_ns}
              for p in CARD.rglob('*') if p.is_file() and not any(part.startswith('.') for part in p.relative_to(CARD).parts)}
    plan = dict(card=str(CARD), device=16777241, old_bin_sha256=OLD_BIN, items=items,
                before_files=before, backup=str(backup), physical_camera_verified=False)
    (OUT / 'plan.json').write_text(json.dumps(plan, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'prepared':str(OUT), 'backup_sha256':sha(backup / 'Z_8_0406.bin'),
                      'files':[{k:v for k,v in i.items() if k!='source'} for i in items]}, ensure_ascii=False, indent=2))
else:
    card_identity()
    plan = json.loads((OUT / 'plan.json').read_text())
    assert sha(Path(plan['backup']) / 'Z_8_0406.bin') == OLD_BIN
    assert sha(CARD / 'Z_8_0406.bin') in (OLD_BIN, NEW_BIN)
    for item in plan['items']:
        assert sha(Path(item['source'])) == item['sha256'], item['source']
        dst = CARD / item['destination']
        if dst.exists():
            assert item['destination'] == 'Z_8_0406.bin' or sha(dst) == item['sha256'], dst
    copied, unchanged = [], []
    # Stage/check each complete file before replacement; existing unrelated files are untouched.
    # Put the active firmware last so its companions and test material are already present.
    items = sorted(plan['items'], key=lambda x:x['destination']=='Z_8_0406.bin')
    for item in items:
        src, dst = Path(item['source']), CARD / item['destination']
        if dst.exists() and sha(dst) == item['sha256']:
            unchanged.append(item['destination']); continue
        dst.parent.mkdir(parents=True, exist_ok=True)
        temp = dst.with_name(dst.name + '.v51-copying')
        assert not temp.exists(), temp
        with src.open('rb') as r, temp.open('xb') as w:
            shutil.copyfileobj(r, w, 4 * 1024 * 1024)
            w.flush(); os.fsync(w.fileno())
        assert sha(temp) == item['sha256'], temp
        os.replace(temp, dst)
        copied.append(item['destination'])
        print('COPIED', item['destination'], flush=True)
    os.sync()
    for item in plan['items']:
        assert sha(CARD / item['destination']) == item['sha256'], item['destination']
    for rel, metadata in plan['before_files'].items():
        p = CARD / rel
        assert p.is_file(), rel
        if rel != 'Z_8_0406.bin':
            assert p.stat().st_size == metadata['bytes'] and p.stat().st_mtime_ns == metadata['mtime_ns'], rel
    receipt = dict(completed_at=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                   card=str(CARD), copied=copied, already_identical=unchanged,
                   firmware_sha256=NEW_BIN, previous_firmware_sha256=OLD_BIN,
                   previous_firmware_backup=plan['backup'], copied_to_physical_card=True,
                   physical_camera_verified=False, flashed=False, all_destination_hashes_passed=True,
                   existing_files_preserved=True, files=plan['items'])
    data = json.dumps(receipt, ensure_ascii=False, indent=2) + '\n'
    (OUT / 'receipt.json').write_text(data)
    card_receipt = CARD / 'V51TEST/COPY-RECEIPT.json'
    assert not card_receipt.exists()
    with card_receipt.open('x', encoding='utf-8') as f:
        f.write(data); f.flush(); os.fsync(f.fileno())
    os.sync()
    assert sha(card_receipt) == sha(OUT / 'receipt.json')
    print(json.dumps({k:v for k,v in receipt.items() if k!='files'}, ensure_ascii=False, indent=2))
