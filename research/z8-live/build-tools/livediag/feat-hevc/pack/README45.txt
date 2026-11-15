Nikon Z8 test package: Live v37 H.265 + C64 switch + portrait-video plugin (switch)
Base firmware C3.11, version tag 04.06.p0, identified by SHA-256.
= previous package (C64 switch, MAIN b12501ea)
  + Live's MOV video switched from H.264 to H.265 (HEVC); bootguard build ID regenerated.

1. Live H.265
- In photo mode the Live encoder now drives Wave5 with codec word 1 (HEVC Main 8-bit, 12 Mbps, 1024x682). Before, it was H.264.
- Clips start only at the encoder's own key frames (IDR/CRA). H.264 needed slice-header rewriting to fake IDRs; H.265 doesn't.
  So the key-frame interval is set by the encoder's own GOP (stock parameter 15 frames, about 0.5 s).
- VPS/SPS/PPS come from the encoder headers and are written before every key frame. The SPS is cropped to 682 rows (the encoder encodes 688).
- MOV sample entry is hvc1/hvcC; the Apple Live Photo pairing (content identifier, still-image-time) is unchanged.
- In-camera playback still uses the 45 JPEG preview frames at the end of the MOV (same as v37), independent of the video codec.
- Not changed: capture, frame picking, tear detection, the audio track, Wave5 sharing with movie/playback, and the LIVE/LIVEDATA layout.

2. Everything else is the same as the previous package (C64 switch, portrait video, portrait histogram/waveform).

3. Suggested camera checks
1. After flashing: photo mode, turn Live on, wait for the icon to turn green, then take a few shots (handheld, with motion).
2. Copy LIVE/…/DSC_nnnn.MOV to a computer: it should play, ffprobe should report hevc/hvc1 1024x682, and Mac/iPhone should recognize it as a Live Photo together with the JPG.
3. If no MOV is produced or the icon never turns green: connect USB Debug and read the encoder state (offsets below).
4. Switch to movie mode and record a clip (H.265/H.264 each), switch back to photo, and take another Live shot — confirm Wave5 sharing still works.

4. USB readout (struct live_w5, pointer at live_runtime+0x2388)
- +1053600 hevc_magic 'HEV1' (0x31564548), +1053604 open_rc (1 = HEADERS set too large or parse failed, 2 = VPS/SPS/PPS incomplete)
- +1053608 the NAL types in HEADERS (one byte each, bit 7 = present), +1053612 number of forced I frames
- +1054404 cropped (1 = SPS crop succeeded)
- +1054416 n_au, +1054420 n_irap, +1054424 n_idr, +1054428 n_cra, +1054432 n_trail,
  +1054436 in-band parameter sets, +1054440 parameter-set changes, +1054444 dropped AUD/SEI, +1054448 last VCL type
- If n_irap stays at 1 (only the first frame), the hardware encoder isn't producing periodic key frames, and a press can't find a key frame 1.5 s back.

5. Verification scope
- Host: real HEVC streams (the QEMU x265 sample and a Mac VideoToolbox 1024x688 stream) through the same AU handling + MOV writer;
  a clip starting at a mid-stream key frame decodes; after the SPS crop, the 682-row output is pixel-identical to the top 682 rows of the original decode.
- Build: the unmodified v37 source rebuilt on the same machine reproduces 191e6e4f byte for byte; the HEVC build changes only the Live/playback payload (0x40196e1c–0x401f9685),
  which doesn't overlap the C64/portrait/bootguard layers.
- QEMU: see verification.json.
- Not run on the camera. The hardware Wave5 HEVC key-frame pattern and parameter sets in photo mode haven't been sampled yet; this package is the first sample.
