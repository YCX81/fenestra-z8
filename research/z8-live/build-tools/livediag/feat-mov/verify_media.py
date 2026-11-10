#!/usr/bin/env python3
"""用 ffprobe 和 ffmpeg 完整解码实验矩阵，将可复核的结果写至标准输出。"""
import argparse
import json
import shutil
import struct
import subprocess
from pathlib import Path

from mov_fix import direct_child, handler, parse, sha
from prepare_tests import ROOT, check


def run(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout.strip()


def probe_streams(binary, path):
    fields = ('stream=index,codec_name,codec_tag_string,width,height,nb_frames,nb_read_frames,'
              'sample_rate,channels,duration:format_tags=compatible_brands,com.apple.quicktime.content.identifier')
    return json.loads(run(binary, '-v', 'error', '-count_frames', '-show_entries', fields,
                          '-of', 'json', str(path)))


def decode_hashes(binary, path, crop=None):
    crop_args = ['-vf', f'crop={crop[0]}:{crop[1]}:0:0'] if crop else []
    result = run(binary, '-v', 'error', '-nostdin', '-xerror', '-noautorotate',
                 '-i', str(path), '-map', '0:v:0', '-map', '0:a:0', '-vsync', '0',
                 *crop_args, '-bitexact', '-f', 'streamhash', '-hash', 'sha256', '-')
    hashes = {}
    for line in result.splitlines():
        stream, kind, value = line.split(',')
        if kind in ('v', 'a') and value.startswith('SHA256='):
            hashes[kind] = value[7:]
    check(set(hashes) == {'v', 'a'}, f'{path} 缺音视频解码校验值')
    return hashes


def check_apple_control(source, identifier):
    # 实验件同源的 JPG 由本项目 writer 生成，有固定单条 Apple tag 0x0011 布局。
    jpg = ROOT.parents[2] / 'live-export/DSC_9776/DSC_9776.JPG'
    still = jpg.read_bytes()
    header = b'Apple iOS\0\0\1MM' + struct.pack('>HHHIII', 1, 0x11, 2, 37, 32, 0)
    check(still.count(header + identifier.encode('ascii') + b'\0') == 1, 'Apple MakerNote ID mismatch')
    check(jpg.with_suffix('.MOV').read_bytes() == source, 'paired original differs from source')
    moov = direct_child(parse(source), b'moov')
    marker = next(t for t in moov.all(b'trak') if handler(t) == b'meta')
    check(b'com.apple.quicktime.still-image-time' in marker.one(b'mebx').data, 'no still-image-time key')
    elst = marker.one(b'elst').data
    check(elst[:8] == struct.pack('>II', 0, 2), 'unexpected edit count or version')
    start, empty, rate, duration, offset, rate2 = struct.unpack_from('>IiIIiI', elst, 8)
    check((empty, rate, offset, rate2) == (-1, 65536, 0, 65536), 'unexpected metadata edit mapping')
    meta_sample = direct_child(parse(source), b'mdat')
    table = marker.one(b'stco').data
    sample_offset = struct.unpack('>III', table)[2]
    check(meta_sample.start < sample_offset and source[sample_offset:sample_offset + 9] ==
          b'\0\0\0\x09\0\0\0\x01\xff', 'unexpected still marker payload')
    return {'jpg': str(jpg.relative_to(ROOT.parents[2])), 'jpg_sha256': sha(still),
            'identifier': identifier, 'movie_timescale': struct.unpack_from('>I', moov.one(b'mvhd').data, 12)[0],
            'marker_start': start, 'marker_duration': duration}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ffprobe')
    parser.add_argument('--ffmpeg')
    parser.add_argument('--if-available', action='store_true', help='供生成器使用，运行已安装的探针并打印摘要')
    args = parser.parse_args()
    bundled_probe = ('/Applications/Parallels Toolbox.app/Contents/Frameworks/ToolboxCommon.framework/'
                     'Versions/A/Resources/ToolboxCommon.bundle/Contents/MacOS/ffmpeg/ffprobe')
    bundled_decoder = '/Applications/Downie 4.app/Contents/Resources/ffmpeg'
    probe = shutil.which(args.ffprobe) if args.ffprobe else shutil.which('ffprobe') or shutil.which(bundled_probe)
    decoder = shutil.which(args.ffmpeg) if args.ffmpeg else shutil.which('ffmpeg') or shutil.which(bundled_decoder)
    if not probe:
        if args.if_available and not args.ffprobe:
            print('ffprobe 未安装；已执行内置结构与媒体验证。')
            return
        parser.error('ffprobe not found, specify --ffprobe PATH')
    if not decoder and (args.ffmpeg or not args.if_available):
        parser.error('ffmpeg not found, specify --ffmpeg PATH')
    out = ROOT / 'tests'
    manifest = json.loads((out / 'current-manifest.json').read_text())
    source = (ROOT / 'live-sample.MOV').read_bytes()
    check(sha(source) == manifest['source']['sha256'], 'source changed')
    result = {'ffprobe': run(probe, '-version').splitlines()[0],
              'ffmpeg': run(decoder, '-version').splitlines()[0] if decoder else None, 'cases': []}
    for case in manifest['cases']:
        path = out / case['file']
        check(sha(path.read_bytes()) == case['sha256'], f'{path} hash mismatch')
        info = probe_streams(probe, path)
        streams = info['streams']
        video = next(s for s in streams if s.get('codec_name') == 'h264')
        audio = next(s for s in streams if s.get('codec_name') == 'pcm_s16le')
        tags = info['format']['tags']
        kinds = [s['codec_tag_string'] for s in streams]
        identifier = tags['com.apple.quicktime.content.identifier']
        geom = [video['width'], video['height']]
        vtrack = next(t for t in case['tracks'] if t['handler'] == 'vide')
        atrack = next(t for t in case['tracks'] if t['handler'] == 'soun')
        check(geom == vtrack['avc1_size'] and int(video['nb_read_frames']) == vtrack['samples'],
              f'{path} video disagreement')
        check(int(audio['nb_frames']) == atrack['samples'] and audio['sample_rate'] == '48000'
              and audio['channels'] == 2, f'{path} audio disagreement')
        check(('mebx' in kinds) == ('remove_mebx' not in case['changes']), f'{path} metadata track disagreement')
        full = decode_hashes(decoder, path) if decoder else None
        if not result['cases']:
            base_hashes, active = full, geom
            result['apple_control'] = check_apple_control(source, identifier)
        region = full if not decoder or geom == active else decode_hashes(decoder, path, active)
        check(region == base_hashes, f'{path} active picture or audio differs')
        check(identifier == result['apple_control']['identifier'], 'MOV content ID changed')
        result['cases'].append({'file': path.name, 'sha256': case['sha256'],
                                'streams': kinds, 'dimensions': geom, 'video_frames': int(video['nb_read_frames']),
                                'pcm_sample_frames': int(audio['nb_frames']), 'decoded': full,
                                'active_dimensions': active, 'decoded_active': region})
    if args.if_available:
        print(f"PASS：ffprobe 检查 {len(result['cases'])} 件。"
              + ('音视频完整解码与有效区域哈希一致。' if decoder else 'ffmpeg 未安装，未做像素哈希对照。'))
    else:
        print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
