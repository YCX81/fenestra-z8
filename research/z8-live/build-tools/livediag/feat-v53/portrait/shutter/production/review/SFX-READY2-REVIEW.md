# Ready2: second sound input is missing, completion includes cancellation

Exact firmware is final F `b0cd7c40c3c6ffdf07b87fcbab20b98fbe9ed0c2a848550855d989b9e64fd8b0`. Read-only evidence is copied in `ready2/` from `v53f-audio-pcm-ready2-capture`. No production source, timeout, native shared state or active emulator was changed.

The output MOV has 96 frames / 3.2032 seconds and 153753 PCM frames. `pcm-summary.json` reports its last 75335 PCM frames silent. The first silent output sample is frame78418, at clip-relative1.633708 seconds. Absolute clip start90202397us plus this offset is91836105us; this agrees within one48kHz sample with the existing duck start (`press91886119us - 50000us`). It does **not** alone timestamp microphone STOP. At the paused checkpoints, `audio.next_index=4411172`, or91899416.7us. The clip ends93405597us; restart anchor is94131402us. Thus the existing150ms duck explains the beginning of the zero run, while the sound-owned microphone gap covers its remaining tail. There is no new PCM before the clip's end to replace that deliberately bridged gap.

The actual service requests (`audio-events.jsonl=on`, one-based lines) are:

| Line | Original request | Observed value |
|---|---|---|
|369|BUFFER4318001a, first sound|a15c19f00,4800bytes,count1|
|374|40c16674 first DMA install|single27,wordcount04af,sourcec5c19f00,FIFO0e200104|
|380/382|OPEN14 / BUFFER1a, second sound|a15aab8b8,0bytes,count1|
|387|40c16674 second DMA install|single27,wordcountffff,sourcec5aab8b8,FIFO0e200104|
|389/390|STOP16 / driver107|contains original cancellation04100032|
|393..399|microphone start sequence|104/10b/101/102/109/106 followed by RX request|

The second START's latest IRQ timestamp is92005420us and the cancellation's is94037940us (difference2.032520seconds). This strongly matches the adapter's two-second bound, but this older observer did not record `shutter.began` or `cancel_posted`; exact reason selection remains inferred. The next driver records them. `completed=2` includes cancelled native operations and is not evidence of two natural sound endings. The zero-byte native request actually reaches the DMA constructor: its16-bit count underflows to65535, describing262144bytes, exactly the separate original-ARM counterexample. This malformed model input must not be granted a successful short-audio IRQ.

## Why tone2 is zero

Native tone1 and tone2 have different sources. Tone1 is a fixed4800-byte sample copied from MAIN43fba590 to a15c19f00. Tone2 is resource2 of external resource4e. `40e34184` calls original resource lookup4e, loads the offset from header+1c and the u16 byte length from header+20, and initializes `a038feb08` / `a038feb10`. Later `40e3469c` and `40e348f4` read those fields. These native ranges are byte-identical in delivered G and final F.

The original descriptor maps resource4e to bank24, source ROM46460000, extent20000, effective bytesee00. Current boot inputs do not supply this region; MAIN ends44f6505d. The QEMU ROM allocation leaves that unused region zero. A scoped file inventory of local migration firmware/references, WinLab WSL firmware/notes/resources and the Windows desktop mirror found no identifiable bank24 dump. This is not a claim that no such dump exists anywhere.

`test_sfx_resource.py` executes the original G and F lookup/init/getters, without any sound request: an explicitly zero resource produces tone2 length0; synthetic4800 and30720 header controls produce the correct data pointers and lengths. This demonstrates missing boot resource is sufficient to produce the observation, independent of delaying the new adapter. It does not substitute for a direct pre-SFX live-RAM cache snapshot.

## Explicit next input

`../../resource-fixture/manifest.json` describes a synthetic resource4e. The header uses the proven offset/length fields and points to4800bytes copied exactly from the official MAIN's built-in tone1. It is **not** recovered tone2 data. Only a startup ROM loader at46460000 supplies it. SHA256 `eabd3f39b2b3e0f427d27103826e26e7d1f7db46fc1fcd172bbaad9e80f01ebc`;60928bytes. Actual G/F initialization and length/duration getters accept it as4800bytes/25ms (`fixture-arm.json`).

Whole-service natural completion still requires a narrowly supported single27/4800B output model that consumes the actual RAM bytes. The current paired192000B REC model does not prove this path. No firmware change or fabricated completion is justified by this missing-resource experiment. Physical shutter timbre, speaker output, ADC/DAC latency and camera recording gap remain unverified.
