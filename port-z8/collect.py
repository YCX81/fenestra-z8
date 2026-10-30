"""List every Z9 C5.32 address the Fenestra sources use (hex and decimal), with where it appears."""
import json, re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FILES = (sorted((ROOT / 'src/platform/z9_c532').glob('*')) +
         [ROOT / p for p in ('host/build_z9_core.py', 'host/platform_profiles.py', 'host/displaced.py',
                             'emulation/native_ui_dispatch_sites.py', 'emulation/native_font_sites.py',
                             'host/ptp_contract.py', 'host/verify_native_namespace.py', 'host/verify_build_identity.py')] +
         sorted((ROOT / 'src').glob('*.[chS]')) + sorted((ROOT / 'src').glob('*.inc')))
HEX = re.compile(r'0x([0-9a-fA-F]{8,9})\b')
DEC = re.compile(r'(?<![0-9a-fA-Fx])(10[7-9]\d{7}|110\d{7})(?!\d)')


def section(path, line, text, lineno):
    """For shared files keep only the Z9 part; ZR profile addresses are not ours."""
    return True


def main():
    found = {}
    for f in FILES:
        if not f.is_file():
            continue
        rel = f.relative_to(ROOT).as_posix()
        zr_only = False
        for n, line in enumerate(f.read_text(errors='replace').splitlines(), 1):
            if rel.startswith('host/') or rel.startswith('emulation/'):
                # skip lines that are clearly the ZR profile
                if "'zr-c110'" in line or 'zr_c110' in line:
                    zr_only = True
                if "'z9-c532'" in line or 'z9_c532' in line:
                    zr_only = False
                if zr_only:
                    continue
            vals = [int(m, 16) for m in HEX.findall(line)] + [int(m) for m in DEC.findall(line)]
            for v in vals:
                if 0x40160000 <= v < 0x46000000 or 0xa00000000 < v < 0xa10000000:
                    found.setdefault(v, []).append(f'{rel}:{n}')
    json.dump({hex(k): v for k, v in sorted(found.items())}, sys.stdout, indent=0)


if __name__ == '__main__':
    main()
