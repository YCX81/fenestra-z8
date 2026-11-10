Read-only, in Chinese. No modification, no USB. Read history/astra-b7imcn17p.md first. Labels: proven by disassembly / inference. bg34.main (Nikon Z8 MAIN, base 0x40160000), src-fix31/ (Fenestra core headers under ui/core/include), W5-BRINGUP.md.

Goal: an independent visible marker for a diagnostic firmware, showing which stage REC reached, surviving a 20 s hang and reboot by being seen or filmed by the user. It must not depend on Live's worker task, the card file lock (0xa02e81dd8) or the display/UI task that may be blocked. Must be callable from the bring-up hook context (MOVRS task) and ideally from any context, with short, non-blocking register writes.

Find candidates and rank by safety, with exact addresses and arguments, and how you know:
1. Card access lamp (the Z8 has a memory-card access indicator on the back): how stock drives it (GPIO/PMIC/EX MCU command), function address, register, polarity, whether stock's card activity would overwrite it during REC, whether direct control blocks.
2. AF-assist lamp, self-timer lamp, or other front LEDs, and any button backlight / top LCD backlight: driver path; is it via the EX MCU serial link (asynchronous, via a task that could be blocked)?
3. A direct MMIO write such as a GPIO at 0x0d...; must be proven not to hit 0x154xxxxx and proven safe.
4. Any other independent channel: UART/debug output, a RAM region proven to survive a watchdog or warm reset (find boot/reset code that clears or preserves regions; early boot initialisation, retained area, reset cause register and where stock stores a panic/exception record; is there NOR writing on fatal errors, BINLOG_N.BIN path 0x40c06fe0 at 0x464e0000?).
5. Beeper through a direct path.
For the top choices give a minimal C sketch for a stage encoder (for example blink-count or on/off pattern on one lamp), which contexts it is valid in, how long it takes, and what could go wrong. Say plainly if nothing is proven safe, and if so the next step.
