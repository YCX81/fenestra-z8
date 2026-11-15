#!/usr/bin/env python3
"""Summarize the isolated passive sound QEMU runs without claiming audible sound."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct
import zlib

def summarize(root):
    rows = [json.loads(line) for line in (root / "live-checkpoints.jsonl").read_text().splitlines()]
    final = rows[-1]
    raw = (root / "lv-observer.dat").read_bytes()
    header = struct.unpack_from("<12Q", raw)
    magic, version, size, capacity, head, overwritten, _, _, errors = header[:9]
    assert magic == 0x4c564f4253455256 and version == 1 and size == 72
    records = [struct.unpack_from("<8Q2I", raw, 96 + (i % capacity) * size)
               for i in range(max(0, head - capacity), head)]
    sound = [record for record in records if record[-1] >= 200]
    runtime = final.get("runtime", {})
    fields = ["requested", "stream.encoded", "transaction_count", "transaction_error",
              "audio.active", "audio_starts", "audio_tracks", "audio_skipped", "audio_error"]
    files = []
    configs = []
    for path in sorted((root / "files").rglob("*")):
        if path.is_file():
            files.append({"path": str(path.relative_to(root / "files")), "bytes": path.stat().st_size,
                          "sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
            if path.name.upper() in ("LIVE-Z8.0", "LIVE-Z8.1"):
                data = path.read_bytes()
                if len(data) == 64 and data[:4] == b"FLP1":
                    configs.append({"path": path.name, "generation": int.from_bytes(data[16:24], "little"),
                                    "requested": int.from_bytes(data[24:28], "little"),
                                    "mute": int.from_bytes(data[28:32], "little"),
                                    "crc_valid": zlib.crc32(data[:60]) == int.from_bytes(data[60:], "little")})
    return {"run": str(root), "final_label": final["label"],
            "finished": (root / "cleanup.json").exists(),
            "runtime": {key: runtime.get(key) for key in fields},
            "sound_state": final.get("sound_state"),
            "gate_seen": sum(record[-1] == 212 for record in sound),
            "stock_sound_start_ids": dict(Counter(str(record[3]) for record in sound if record[-1] == 205)),
            "sound_point_counts": dict(Counter(str(record[-1]) for record in sound)),
            "sound_events": [{"seq": row[0], "pc": hex(row[2]), "args": [hex(v) for v in row[3:7]],
                              "point": row[-1]} for row in sound],
            "observer_overwritten": overwritten, "observer_register_errors": errors,
            "files": files, "saved_configs": configs,
            "limitations": ["QEMU sound API calls do not prove audible output on hardware",
                            "audio_tracks == 0 means no recorded audio track was exercised"]}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runs", type=Path, nargs="+")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = {"runs": [summarize(root) for root in args.runs]}
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    for result in report["runs"]:
        print(result["run"], result["runtime"], result["stock_sound_start_ids"])

if __name__ == "__main__":
    main()
