# v52 integration status

**Current final delivery:** v52 production G, MAIN ed1e8496…aa613; local ZIP `/path/to/Nikon/delivery/Z8-v52-Live-EVF-ed1e8496.zip`, BIN e4a61d2a…890f0f. Final source `integration-g/work`, working source synchronized. Core3110, close/reopen repeat3137 and native4K237 checks passed. Physical validation pending; shutter behavior not repaired. SD installation now completed and safely ejected; original content verified and card Live preference saved On. See sd-card-20261114-v52/receipt.json. See `integration/FINAL.json`, `results-g/verification.json` and final section below. Earlier candidates/remaining lists are historical.

2026-11-14. Authoritative hardware observations are in HARDWARE-OBSERVATIONS.md.

Baseline: final v51d MAIN aa6e94e0042fb445c666de73c0f6b9e7e569010b7993a3ac3da0979b174a9433, source feat-v50/integration-writer/v51d-work. No camera/USB/card operations in this repair round.

## Integrated in immutable integration-c/work

- handoff: early playback hold, microphone drain before ACK, strict W5 pool disposal and serialization.
- handoff-recovery: ordinary photo playback also closes Live's own W5. v52b exposed a real new self-wait: IRQ resumed but its own VDI prevented hold release. Failure retained; do not call it an emulator flake.
- audio v4: native request output buffers sized for result writes; stop callback/lifecycle safety; REC waits for microphone release; full supported clip PCM instead of 2.4 s truncation; bounded stop retry and ownership ACK; generation gate rejects stale REC reopening after native handoff.
- CFR: capture scheduling and writer use 30000/1001. Old VFR files remain unchanged. Actual v52b capture is 99 HEVC frames / 3.303300 s, SHA 46bacc4c3b28bcabd2c8e0795b15e1604aba000cfb14ea39bcd6a79fbcb0c9ae, ffmpeg decode passed.
- EVF: original output selector, correct group/output argument order, strict owned empty-root rebind, 40 B core patch after PB link. Actual physical display unverified.
- Passive shutter diagnostics only: prepare/fire/full press/mute entry records. No claim of repaired shutter audibility.
- Root speaker handoff helper reads the registered PB callback only for explicit native handoff. All extra code is linked and extracted separately at 401fd218; the size/meta helpers at 401fd000..401fd218 remain byte-identical.

## Build C

Remote source /path/to/user/fxfix/v52-root/integration-c/work; build /path/to/user/v37build/v52c; composed MAIN /path/to/user/fxfix/v52-root/v52c/q/main.main.

MAIN SHA 4790c3e6138f4f22a5c189988e3ba7338de8f315b9bb3515930b3b97132ac00f. Primary Live 109328 B, extra 2228 B, PB 36848 B. Runtime 23264 B, PB runtime 9352 B. Source SHA manifest integration/source-c.json. Driver-c adds passive fields only; tests retain original input/deadline/return assertions.

Core C completed with a retained-own-VDI failure: 45 REC frames, new IRQ but no new encoding. A passive-only repeat v52c-close-diagnostic is running to read actual W5 close status, using the unchanged C MAIN. A candidate retaining the W5 during ordinary REC and allowing resume only for strictly verified exclusive Live ownership is under review; explicit native playback still must preempt/dispose. Do not clear close_failed or relax the recovery assertion.

Native playback on C: 4K reference completed 95 decode pictures and audio conversion. Actual CFR capture parsed and decoded its first frame, then hit an unchanged unknown display MMIO boundary 0x15421104; no blanket tolerance was added. Full CFR decoder correctness separately passed ffmpeg. A prior native LiveOn run had requested=1 but encoded=0: it cannot validate native takeover from an active Live encoder.

Linked C Live→registry→PB audio handoff passed 15 ARM cases using the actual final linked code, with no service stubs. The audio agent is running a separate PB_AUDIO_TEST build that sends a zero-duration silent service transaction; it is not a delivery image or audible/DMA validation.

For core runs, root result.firmware_sha256 is the prepared stock dependency, not executed MAIN. Use live_a.executed_main_sha256 AND the actual command -kernel path. Native wrapper identity is independently pinned.

## Remaining

- Finish C core and native regressions; retain failures and exact identity.
- Portrait agent: LCD REC/timer positions and missing EVF normal/small video portrait tables. These are not yet in C.
- Integrate portrait with strict expected bytes, re-ID and regenerate aggregate manifest; validate both portrait directions/layout variants.
- Audio integration protocol test if supported by emulator. QEMU does not establish microphone quality, DAC audibility or hardware coexistence.
- Package only a concretely verified test candidate. Shutter root cause and physical EVF/audio/handoff validation may still require new camera observations; label them explicitly.

No v52 package has been delivered or placed on the card. The card is currently absent.


## Final E candidate (current)

E corrects the newly discovered stale AVC-only close guard: native HEVC instance type is 1, AVC is 3. Independent native instruction tests confirm the type mapping and shared EOS/FINISH path. The original arena field is 0xa02d3d950 (VDI struct base 0xa02d3d928 + 0x28); D incorrectly checked the b0 partition and was intentionally stopped/superseded, with evidence retained. E uses the actual original arena and saved RUN instance binding. Production uses strict retained-owner resume for ordinary REC; the independent Eclose diagnostic uses C's worker close to exercise real HEVC EOS/EXIT/disposal and reopen.

Immutable integration-e/work and integration-eclose/work; E w5enc SHA d3f454de19bb789acd44b1bce169e254b473d40d6353cf2de7e419e69070b9bc. Combined portrait overlay 419 records / 3518 bytes is now in compose. Tools forbid raw intermediate --stock packaging.

Production E MAIN /path/to/user/fxfix/v52-root/v52e/q/main.main SHA 0008f196cf3e69edb12443e74f9b5466be762adc88c84a84c2192d1030437963, buildID25c3dd5b. Core v52e-final-rec PASSED: 3066 checks; one 96-frame 3.2032s CFR MOV; REC45 frames; return IRQ1556/encoded551 to IRQ2050/encoded841; final2146/903 hold0. Full ffmpeg decode passed. Actual MOV SHA add968b243394b516ba15bf1d2c4f7b9f20d94481fa9333ab79c9e90b2eb49e1 (silent emulator input; candidate DSC_9051.MOV). /v52e/core-validation.json records exact kernel identity. Linked Live-registry-PB handoff 15 ARM cases pass.

E binary audit PASS at portrait-rec/review/final-binary-e.json: entire aggregate reconstructs MAIN; all frozen files, ELF sections, hooks, fonts, size/meta gap and portrait verified. Expected package SHA 3fb7db823d4029c149d245aaa12d265452a6913883e59dbe93f43e52cb62aacf, 99051117 B. pack52.py now additionally asserts this independent package hash.

Eclose MAIN 3b976636e4b67c86a9e93b0d82c598d23a7ea3e73c4733b3a7b82b27f252e7cb: successful real codec close/free already observed (hw0, VDI0, client4ff, worker0); final resume result pending playback agent. Final E native4K lane also assigned to playback agent. Do not package before those results are reviewed. Root sessions56994/38407 are production E/Eclose; logs /path/to/user/fxfix/v52-root/v52e-final-rec.log and v52e-hevc-close.log. Driver-e adds passive W5 snapshots only.

Audio macro-only C run completed real open/buffer/start/stop callbacks, errors0, resource release, REC45; overall C recovery failed due W5 guard. No audible/full DMA claim. Shutter EV60 four points actually exported from C/Cdiag card logs; shutter audibility remains unresolved, no behavioral shutter fix. Source/runtime audio is identical in E.

README52.txt and pack52.py prepared/uploaded under remote v52-root. Final verification.json still to create after Eclose/native results. No v52 delivery package or physical-card write yet.

### Eclose follow-up: release passed, reopen FAILED; release remains blocked

The Eclose run finished exit1 with the unchanged recovery assertion. Before IRQ1564/encoded554; final IRQ2060/encoded554, hold0. Real EOS/EXIT/dispose succeeded earlier (hw0/VDI0/client4ff/worker0), but the new instance stops at WSTATE2 across the entire recovery window; new VDI16010000/client4map0/slot_used1/instance(1,1), close_failed0 and frames0. Capture WARM/reason1 and stream.error0. This is an init/reopen failure, not the old hold self-lock. Playback agent now traces stderr/model; audio agent independently traces actual native OPEN/config/register/headers/state2 paths. Do not package E merely because ordinary REC recovery passed. Final native E4K lane is also owned by playback agent.

Root collect_core.py gathered exact production-E proof and independently ffmpeg decoded the actual96-frame MOV; its final snapshot comes from live_checkpoints[label=final], not final_status. The Eclose failure is still preserved and must be resolved or meaningfully bounded before final candidate release. Root exec session IDs may have expired after tool updates; use remote exit/result files and owned_qemu_stopped rather than assuming a new failure or restarting.

Root started unchanged-Eclose passive probe v52e-open-diagnostic with driver-e-open. Observer SHA 852acba05487adc63b4fa1da1bf36631dc19d1d65b053d60479ab03228370088; it adds SET_PARAMS return x8 at40c38a74 (kind401), HEADERS service x0 at40c33df0 (402), command x0 at40c3b6d4 (403), and passive RAM status window. It leaves original inputs/deadlines/assertions unchanged. First compile used a nonexistent include path and failed before launching; corrected include is merged/work/qemu-10.2.4/include/qemu. The sole successful run is root session87852, remote log/exit v52e-open-diagnostic.*. No delivery.

### Reopen cause discriminated by unchanged-Eclose observer

The sole v52e-open-diagnostic run ended with the original no-new-encoding assertion (exit1, owned QEMU stopped). First open:401 SET_PARAMS x8=0 →403 VPU HEADER x0=0 →402 HEADERS x0=0. Second open:401 x8=0 →402 x0=0xffffffff, with no second403. Later WSTATE2. Thus SET_PARAMS returned success before the worker was ready, and HEADERS was rejected before any hardware HEADER call. The final E native4K lane independently PASSED95 frames/11 LPCM commands with exact E kernel SHA (native-playback-e/evidence/summary.json). This does not resolve Live On hardware handoff.

F/Fclose source trees created from E/Eclose, not built yet. Portrait agent is adding a bounded actual-state/ownership readiness wait following successful SET_PARAMS; no re-open or native event/result clearing. Audio agent is fixing a separate proven lifetime bug: HEADERS stores output pointers and may write after timeout, so the reply must live in retained scratch rather than the caller stack. Generic partial-open cleanup is not considered solved: current source pins invalid state, but live_stream.release retains encoder.hw, so repeated W5 pool allocation/overwriting was not demonstrated. F driver inherits only passive E-open observations; runtime/PB offsets independently regenerated and identical.

### F candidates built; final three QEMU lanes running

Combined codec SHA84b755280f649a315edd978337536c646a4d721c439ce4a211b33af397c8ae00 adds readiness after successful SET_PARAMS plus persistent HEADERS reply. Readiness90 actual ARM cases passed; lifetime actual ARM baseline overwrites28stack bytes versus candidate0. No generic partial-open retry/free was added. F MAIN ceefa749d298066d3666c083d4a15a1985939f22ca13ebd8d80244b28963857b, IDa737dc65, live110544/free48, extra2064/free1496, PB36848/free4112. Fclose diagnostic MAIN fc69fbf782bfae754e6e11c95095883d3e805d0c154bb6d22422c24f47523d86, ID5f70cbe4, same codec but ordinary-REC worker close enabled. Do not deliver diagnostic.

Root launched v52f-final-rec (session9216) and v52f-hevc-close (1603), driver-f with same passive observer SHA852acba0 and original deadlines/assertions; remote v52-root logs/exit files. Playback owns final F native4K lane /path/to/user/fxfix/v52f-native/run-reference (session98000). Portrait owns independent finalF binary/owner audit. integration/work synced to F codec; immutableF/Fclose402-file manifests preserved. No final package yet.

### F superseded: incorrect mutable result-slot prerequisite

F/Fclose first initialization regressed: SET_PARAMS401=0, no HEADERS402/403, state2 with encoded0, capture warm/reason1. Root requested owned-stop on both core runs with explicit owner-stop-reason.json; no pass claim. F native4K continues independently. Root/agents found prior successful E snapshot (536 encoded frames, state4) already has a02d32788=-1. Actual native40c3b17c..1a0 handles a repeated INIT while state!=0 by overwriting2788=-1 and posting20, whereas successful init publishes state2. Therefore readiness must not treat2788 as a stable success flag. Portrait is correcting this and adding the actual native post-success overwrite counterexample; audio independently checks state2-only success semantics. G will preserve rc check, pre-state0, bounded state readiness, all owner/binding checks, and persistent HEADERS reply. No F package.

F binary/ELF audits passed but do not override this functional failure. Source manifest initially differed only because macOS tar re-encoded27 AppleDouble files; exact base64 restored metadata bytes in source/build trees, recorded remote f-source-metadata-transport.json. Original402-file audit then passed without weakening; actual compiler inputs/MAIN/ELF never changed. Future tar uses COPYFILE_DISABLE=1 and --no-xattrs.

### G built and running, replaces F invalid readiness prerequisite

G keeps exact owner/binding, pre-state0, one SET_PARAMS rc0 and bounded state0/1→2 readiness. It removes only the disproven persistent2788==0 prerequisite. Independent actual-native failure tests (audio/native-w5-ready-failures.json) show6 real failure branches remain0/1, only success publishes2; subsequent refused INIT can set2788=-1 while HEADER remains valid. Persistent HEADERS reply retained. Codec merged SHA df8ebae5aacd18de39c07b286447255ab2a316647f3b1c454f914ea80ef78f8e. Source402 manifests g/gclose.

G MAIN ed1e8496fccde443412e527a252de5029a55c749053b9b046da42bfc454aa613 ID8fb1ba4c, primary110528/free64, extra2064. Gclose MAIN5a8a306dc93e9853bed7a7c57a37e768245a1a61ddefe9e400edb9fbf68179f7 IDe5d18b55, primary110320/free272/extra2228. Root lanes v52g-final-rec(session31472), v52g-hevc-close(93123); playback G4K session87284 under /path/to/user/fxfix/v52g-native. Driver-g retains same852acba0 passive observer, identical ABI and all original assertions/deadlines. Portrait exact binary/source/packet audit pending. collect_reopen.py now requires full corepass plus explicit pb-view hw0/VDI0/worker0/client4ff, two successful nativePARAMS/HEADER calls, and only runtime.c difference between production/diagnostic. No delivery yet.

G production core v52g-final-rec completed naturally PASS: final IRQ2162/encoded906, recovered IRQ2066/encoded845, REC45, all original assertions. Root collect_core runs exact identity/ffmpeg checks now. Gclose first run failed before any encoder initialization: IRQ464 froze after menu exit, requested1/hold0/hw0/VDI0; no401/402/403 at all. Natural error No verified Live photo owner for REC, owned_qemu_stopped true. This failure remains recorded, portrait independently tracing stop/model evidence. Root starts unchanged repeat v52g-hevc-close-repeat session54572; it must actually close/reopen before any release claim. No relaxed inputs/deadlines/guards.

### Final delivery completed

Production G and diagnostic Gclose-repeat completed with original assertions. Actual close report proves pb-view hw0/VDI0/worker0/client4ff, then second nativeSET_PARAMS/VPUHEADER/HEADER service all0 and encoded552→844 (final905). Production captured98 HEVC frames at30000/1001,3.269933s, silent emulator input; full ffmpeg decode passed. Delivered as TEST_MOVIES/DSC_9051.MOV. Native4K exactG completed95frames/11LPCM commands and237checks. Final linked ARM66owner+46ready+15audiohandoff and402source/binary/package audits passed.

Local ZIP SHA97a64460ce2bb7b9c65f9b417587f6fc6412a47db145eca1db343b634b8f56dc; BIN SHAe4a61d2af3480f66b37a83ec967607a179f18fbbc454c2df7673285e80890f0f,99051117B. ZIP CRC, all7 manifest-listed file hashes, exactBIN andMAIN metadata verified again after downloading; archive and extracted folder are in delivery. SD check still only Macintosh HD, no card write. No camera/USB/flashing performed.

Initial Gclose no-first-IRQ failure remains explicitly in package verification/readme. Frozen analysis `portrait-rec/review/G-STARTUP-COMPARISON.md` proves stop waits succeeded, restart MMIO issued, first codec never entered; laterA2 comes from an asynchronous flag with unknown setter/initial-stop cause. Do not relabel that run as passed or as a proven model-only bug. Unchanged repeat passed.

Hardware next: LiveOn native ordinary-video playback and return to newLive; LCD/EVF capture and REC display/audio; portrait video REC/elapsed positions in LCD and bothEVF sizes; newLive microphone quality and timing. Shutter still passive diagnostic only; preserve newMOV/JPG plus LIVEDATA/LIVELOG.BIN and LIVEPREV.BIN for unresolved shutter behavior. Generic partial-open failures pin resources safely but do not have general recovery. No further offline QEMU work is pending for this delivered test candidate.

### SD installation at user request, completed 2026-11-14

Verified current SD volumeUUID9D59454C-7746-3814-8B03-E2B56691AE15/disk4s1/exFAT. Copied exact existing G BIN e4a61d2a…890f0f to root, added DSC_9054.MOV inDCIM/108NCZ_8 because9051 already has userJPG/NEF, and added V52TEST ZIP/README/verification. Original9041–9044 test hashes still matched and were retained. Backed up previousroot firmware and bothLive config slots onMac under sd-card-20261114-v52/backup-before-v52.

Live default request implemented through existing saved startup preference, not a new firmware rebuild: selected oldgeneration20/slot0 wasalreadyrequested1; wroteinactive slot1 asgeneration21/requested1, preservingmute1 andallreserved/settings bytes, checksumrecalculated. Bothvalidslots nowOn; manualfuturemenuOff remainsrespected. All221 non-target originalfiles includingphotos/settings/plugins/BOOTflag/DCFindex/logs SHAverified unchanged. SixnewmacOS AppleDouble sidecars were explicitly inspected bynames/magic/4096size/exacthash; noexistingfile excluded. Allsixwrite targets verified. Card was synced and safelyejected(disk4). No camera flash performed.
