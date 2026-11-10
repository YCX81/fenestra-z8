Read-only feasibility analysis, in Chinese. Do not modify anything, no USB calls. Label each claim: proven by disassembly / inference. Files: bg34.main (running Nikon Z8 MAIN C3.11-based, load base 0x40160000), src-fix31/ (Live photo payload; codec/live_transaction.c writes the files, capture/ builds the preview), pbfix/live-playback-ui-v28/ (playback overlay: runtime.c, sync.c, native_ui.c, ui.c, preview.c), W5-BRINGUP.md, W5-INIT-REPORT.md, v22..v31 patches.

Goal from the owner: remove the separate LVP preview file (1024x682 I420 x ~45 frames, ~47 MB, ~7 s of write time per shot, worse quality than the MOV, playback only). In-camera playback of a Live must use the original MOV natively. Today: each shot writes A:\LIVE\<dir>\DSC_n.MOV (H.264 High, 1024x680, ~30 fps, about 3 s, AAC or PCM audio per live_audio_mov.c), A:\LIVE\<dir>\DSC_n.JPG, LIVEDATA manifest/LVP, and copies the MOV to A:\DCIM\<dir>\LIV_n.MOV. The owner reports LIV_ files do not show in the playback list. Live view of the JPG shows, then the overlay draws LVP frames at 30 fps over the still.

Stock movie playback is known to use Wave5 as clients 2/3 via the MOVS module around 0x40cf9810 (plan table 0x42a00928), core A.

Investigate and answer:
A. How stock playback builds its list: what makes a MOV appear (DCF naming rules, prefix, folder, database registration, file attribute, sidecar, header/atom check, resolution/codec table). Why would LIV_nnnn.MOV written by our code be absent? What precise name/atoms/registration would make it appear? Does a restriction on size 1024x680 exist; find the table of playable sizes/rates and the codec checks.
B. Options to play the MOV natively, ranked by risk and effort:
  1. Have stock player play a specific file from the Live still, e.g. entry point and arguments to open a movie file in the MOVS module from playback, including full-screen behaviour and how to return.
  2. Decode in the overlay through the stock MOVS demux/decoder path into a buffer and draw over the still with existing overlay (inline autoplay).
  3. Direct use of the Wave5 service for decode as a client.
  4. Make the file a standard stock-recognised movie (for example encode with a stock size such as 1920x1080 or a pad) if the table requires it. Note live view source is 1024x682 and hardware encoder does not scale.
For each: exact addresses, arguments, state required, conflict with Live's own client 4 encoder and with the unresolved Wave5 hand-back problem (see W5-BRINGUP.md section 7 and the fact that a stock EXIT after Live is delicate), audio, portrait orientation, and which unknowns need a read-only hardware check.
C. Recommend one design. List exact code to remove (LVP writer, pbfix paths, pool memory), the expected saving, and the smallest first experiment, preferably one that can run with USB debug calls and no flash (note: USB Debug attached blocks playback, so say how to do it).
If something cannot be determined, say so.
