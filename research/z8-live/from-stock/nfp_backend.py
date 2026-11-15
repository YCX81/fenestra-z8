"""Run a pinned public NFP toolkit in its own interpreter process."""
import argparse
import binascii
import hashlib
import json
from pathlib import Path
import sys

STOCK = 'd67413c2ede6b9ce8887175f20700bd9fbad2a973a4820968c45503d15d80d96'
MAIN = '2b43a31c9de838f84b12416de4348a7004e65be07f853ec56ec2b10f3a8e3853'
FINAL = 'fecbe07e3a0ea2a9136b8e6a396e06b81d3d4e81eda7881c4b98ae20653be1f4'
PACKAGE = 'a6e1d92ef0c52fc4af066eb128460f8c01415e282ade12070e1c4bf5d70a6cde'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('decode', 'r1', 'rc2', 'pack'))
    parser.add_argument('--toolkit', type=Path, required=True)
    parser.add_argument('--stock', type=Path, required=True)
    parser.add_argument('--main', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    raw = args.stock.read_bytes()
    if sha(raw) != STOCK:
        parser.error('requires the complete original Z8 C 3.11 package')
    sys.path.insert(0, str(args.toolkit.resolve()))
    from z8prores.package import Package
    package = Package(raw)
    if args.action == 'decode':
        data = package.eg[:-2]
        if sha(data) != MAIN:
            raise ValueError('original MAIN identity differs')
    elif args.action == 'pack':
        if args.main is None:
            parser.error('--main is required for pack')
        main_image = args.main.read_bytes()
        if sha(main_image) != FINAL:
            raise ValueError('final MAIN differs from v36')
        eg = main_image + binascii.crc_hqx(main_image, 0).to_bytes(2, 'big')
        data, _ = package.replace_eg(eg, 'eg2070_0406p0.bi')
        if sha(data) != PACKAGE:
            raise ValueError('complete update package differs from v36')
    else:
        from features import build
        features = (['prores', 'precapture', 'pixelshift', 'opengate-nraw']
                    if args.action == 'r1' else ['framing', 'video-quality', 'photo-tools'])
        eg, _ = build(package.eg, '04.05' if args.action == 'r1' else '04.06',
                      'p0', features, code_source='prebuilt')
        data = eg[:-2]
    with args.output.open('xb') as output:
        output.write(data)
    print(json.dumps(dict(action=args.action, sha256=sha(data), bytes=len(data))))


if __name__ == '__main__':
    main()
