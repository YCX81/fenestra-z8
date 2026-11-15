from pathlib import Path
import hashlib,json
D=Path(__file__).resolve().parent
read=lambda p:json.loads((D/p).read_text())
sha=lambda p:hashlib.sha256((D/p).read_bytes()).hexdigest()
pin='aa6e94e0042fb445c666de73c0f6b9e7e569010b7993a3ac3da0979b174a9433'
audit=read('playback/candidate-v51d-review.json')
assert audit['result']=='passed' and audit['main_sha256']==pin
assert sha('integration-writer/v51d/q/main.main')==pin
live=read('audio/qemu-v51d/v51d-summary.json');assert live['exit_code']==0 and live['runtime']['transaction_error']==0
native=read('writer/parser/final-native-results.json')
assert {r['name'] for r in native}>={'final-recorded','final-audio','final-silent'}
for r in native:
 if r['name'].startswith('final-'):assert r['loaded_main_file_sha256']==pin
paths=['playback/candidate-v51d-review.json','writer/v51d-source-portrait-audit.json','writer/nctg-tests/final/tests.json','audio/writer-review/audio-paths/transactions.json','audio/writer-review/audio-paths/final-parser.json','audio/writer-review/audio-paths/audio-integrity.json','audio/writer-review/pb-audio-final-v51d.json','audio/qemu-v51d/v51d-summary.json','writer/parser/final-native-results.json','playback/qemu-v51d-final-recovery.json','playback/qemu-v51d-stock-error.json']
r=dict(version='v51',internal_build='v51d',main_sha256=pin,release_status='offline_test_candidate',core_regression_passed=False,writer_host_passed=True,binary_audit_passed=True,physical_camera_verified=False,copied_to_physical_card=False,
summary='Live拍摄和REC预览通过；最终恢复断言失败。原厂4K格式fixture完整播放；实际1024x682片仅解首帧，后续显示模型缺口。不是可刷版验收。',
source_authority=str(D/'integration-writer/v51d-work'),build_directory='/path/to/user/v37build/v51d',source_audit=audit,portrait_source_commit='8357634600b1a31a9a74fc0c6d0db704a37a9f9b',
writer_patch_sha256=sha('writer/writer.patch'),writer_final_patch_sha256=sha('writer/nctg.patch'),
host_transactions=read('audio/writer-review/audio-paths/transactions.json'),stock_parser=read('audio/writer-review/audio-paths/final-parser.json'),audio_integrity=read('audio/writer-review/audio-paths/audio-integrity.json'),pb_audio=read('audio/writer-review/pb-audio-final-v51d.json'),
final_live_capture=live,final_rec_recovery=read('playback/qemu-v51d-final-recovery.json'),stock_error=read('playback/qemu-v51d-stock-error.json'),
native_tests=native,native_scope={'final-audio':'Final C writer host container/audio/JPEG/metadata with known 4K video substituted for model support; 95 decoded frames and 11 LPCM jobs; audio_output none','final-silent':'Final C writer host 1024x682 silent file; unchanged original video samples; first frame then unmodeled 0x15421104 display','final-recorded':'Actual final v51d QEMU-recorded MOV copied byte-for-byte onto isolated virtual card; original 1024x682 samples'},
prior_recovery_controls={p:read('playback/'+p) for p in ['qemu-v50-recovery.json','qemu-recovery-r3-compare.json','qemu-noobserve-compare.json','qemu-v49-v50-error-compare.json']},
limits=['No physical camera/card/flash use','Microphone-enabled shutter sound unresolved','True Live/native contention and audio output/synchronization not hardware verified','Portrait full video UI blocked by model interfaces; byte and ARM execution validation only','NCTG uses nominal configured FPS; stts retains actual VFR timestamps','Files remain in LIVE directory; no new native DCF registration or REC player substitution'],
evidence_manifest=[dict(path=p,sha256=sha(p)) for p in paths])
(D/'verification-v51.json').write_text(json.dumps(r,ensure_ascii=False,indent=2)+'\n')
print('Verified offline candidate evidence; recovery failure retained')
