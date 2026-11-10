Read-only, in Chinese. No modifications, no USB. Read history/astra-bptes20qq.md first (it defined H1/H2/H3 and the addresses). Labels: proven by disassembly / proven by hardware reading / inference.

New data in snap/ (see snap.py for each range and base address; running firmware v32, stock base same as bg34.main). Two snapshots, both movie mode before REC, USB Debug attached (so live view stopped), cold boots:
- never-*.bin: Live off in menu, never ran.
- ran-*.bin: Live on, photo mode ~5 s, then movie mode. Live's stream is still open as designed in v32 (svc_up 1, slot0 used, client map 4->0, VDI on Live's arena 0x8a2c9b000).
(ran-pool24.bin does not exist; pool24h is class 0x24, base 0xa030136e0. The class written '24' in your earlier answer was hex 0x24.)
Quick diff: bufs, res, movrs, movrs_arena, gate, class_map, usb, u1-u3, view are identical. Both 'bufs' tables are all zero and 'res' equal, in movie mode before REC. pool24h differs in 619 words; svc 602, instpool 29, drv 11, staging +8 (flag 0->1), ucode 3.

Do this:
1. Decode pool24h in both: allocator type, total, unit, free list, free total and largest contiguous free block, and how much Live holds. Use the free-list layout you described; walking pointers outside the dump is impossible, so say how far the dump allows it and give the exact additional RAM read needed if the node records are elsewhere (address list, computed from these dumps).
2. Given bufs/res zero before REC, when and from which class is the REC memory (types 0..3: 225/947/807 MiB) allocated in the path started by the REC? Is it class 0x24 or another, and how much must be free? Does Live's allocation (class 0x24, ~65 MB plus capture/preview allocations) overlap or starve it? Use the class_map values and the code. Conclude for H2 with numbers, or state exactly what else to read.
3. Review svc/drv/instpool/ucode differences: is each difference exactly what a live open stream should cause? Anything unexpected for H1, e.g. a field a full close would not restore?
4. Update the hypotheses. Is the remaining question only H3? If so, say honestly whether any no-flash option remains (for example a further USB-RAM procedure), and if a flash is unavoidable, specify the single minimum firmware change that yields the most evidence, not a fix attempt.
