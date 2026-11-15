"""Rebuild a private byte-bearing manifest from a public hash recipe and local MAINs."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

BASE = 0x40160000


def sha(data):
    return hashlib.sha256(data).hexdigest()


def materialize(recipe, image, baseline):
    if len(image) != recipe['image_bytes'] or sha(image) != recipe['image_sha256']:
        raise ValueError('local image does not match the selected recipe')
    result = dict(recipe['historical_metadata'])
    result['historical_output_sha256'] = result.get('output_sha256')
    result['output_sha256'] = sha(image)
    result['ranges'] = []
    for row in recipe['ranges']:
        address, size = int(row['address'], 16), row['size']
        at = address - BASE
        if at < 0 or size <= 0 or at + size > len(image):
            raise ValueError('range outside image')
        encoding = row.get('expected_encoding', {})
        if encoding.get('kind') == 'zero':
            expected = bytes(size)
        elif encoding.get('kind') in ('B', 'BL'):
            delta = int(encoding['target'], 16) - address
            if size != 4 or delta % 4 or not -(1 << 27) <= delta < (1 << 27):
                raise ValueError('invalid branch recipe')
            opcode = 0x94000000 if encoding['kind'] == 'BL' else 0x14000000
            expected = struct.pack('<I', opcode | ((delta // 4) & 0x3ffffff))
        else:
            expected = baseline[at:at + size]
        replacement = image[at:at + size]
        if sha(expected) != row['expected_sha256']:
            raise ValueError('baseline range differs at ' + row['address'])
        if sha(replacement) != row['actual_replacement_sha256']:
            raise ValueError('replacement range differs at ' + row['address'])
        result['ranges'].append(dict(address=row['address'], size=size,
                                    purpose=row.get('purpose', ''),
                                    expected=expected.hex(), replacement=replacement.hex()))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--recipe', type=Path, required=True)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    baseline = args.baseline.read_bytes()
    if sha(baseline) != 'c770f34014849b2c42f20b9258a32b8959253cfb296e48f83417572781e00864':
        parser.error('expected the local base-c770f340 MAIN')
    result = materialize(json.loads(args.recipe.read_text()), args.image.read_bytes(), baseline)
    with args.output.open('x', encoding='utf-8') as output:
        json.dump(result, output, indent=2)
        output.write('\n')
    print(json.dumps({'output': str(args.output), 'output_sha256': result['output_sha256'],
                      'ranges': len(result['ranges'])}))


if __name__ == '__main__':
    main()
