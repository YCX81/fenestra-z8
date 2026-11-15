#!/usr/bin/env python3
"""Apply the v50 stereo gain fix to the v49-derived Live source directory."""
import argparse
from pathlib import Path

OLD = "if(!e&&got)for(uint32_t i=0;i<*got;i++){int32_t s=(int32_t)out[i]*4;out[i]=(int16_t)(s>32767?32767:s<-32768?-32768:s);} /* v28: +12 dB */"
NEW = """/* got is a count of stereo frames, not int16_t samples. Apply the v28
  * +12 dB gain to both channels throughout the complete returned block. */
 if(!e&&got)for(uint32_t i=0;i<*got*LIVE_AUDIO_CHANNELS;i++){int32_t s=(int32_t)out[i]*4;out[i]=(int16_t)(s>32767?32767:s<-32768?-32768:s);} /* v50 */"""

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    args = parser.parse_args()
    path = args.source / "runtime.c"
    text = path.read_text()
    if NEW in text:
        assert OLD not in text
        print(f"already patched: {path}")
        return
    assert text.count(OLD) == 1, "expected one known v28 gain loop"
    path.write_text(text.replace(OLD, NEW))
    print(f"patched: {path}")

if __name__ == "__main__":
    main()
