#!/usr/bin/env python3
"""离线读取 v4 W5Lnnnnn.BIN；只有准备/入口，不包含 hand-back 返回证据。"""
import argparse
import json
import struct
from pathlib import Path

MAGIC, VERSION, FILE_BYTES = 0x344c4657, 4, 3072
EVENTS = (0, 15)
REASONS = {1:'epoch变化(仅标记)', 2:'shutdown', 4:'arm≠generation/arm=0',
           8:'ready=0', 16:'claimed', 32:'照片模式', 64:'写锁争用'}

def checksum(data):
    h = 2166136261
    for v in data:
        h = ((h ^ v) * 16777619) & 0xffffffff
    return h

def decode(data):
    rows, errors = [], []
    if len(data) != FILE_BYTES:
        errors.append(f'文件长度 {len(data)}，预期 v4 {FILE_BYTES}')
    for offset in range(0, len(data)-511, 512):
        b = data[offset:offset+512]
        if not any(b):
            continue
        p = struct.unpack('<128I', b)
        slot = offset//512
        if slot >= 2:
            errors.append(f'保留扇区 {slot} 非零：不是有效 v4 记录')
            continue
        if p[0] != MAGIC or p[1] != VERSION or checksum(b[:508]) != p[127]:
            errors.append(f'扇区 {slot}：魔数/版本/校验失败')
            continue
        if p[2] != slot or p[3] != EVENTS[slot]:
            errors.append(f'扇区 {slot}：序号/事件非法')
            continue
        q = lambda i: p[i] | p[i+1] << 32
        details = [dict(事件=p[i], 阶段=p[i+1], 错误=hex(p[i+2]), 写入=p[i+3],
                        关闭=hex(p[i+4]), 编号=p[i+5]) for i in range(28, 64, 6)] if slot else None
        rows.append(dict(序号=slot, 事件号=p[3], 事件='准备' if not slot else 'hand-back 前入口',
            地址=hex(p[4]), 计数器=q(6), 会话=hex(q(8)), 客户=p[10], 原始x0=hex(q(12)),
            原始x19=hex(q(14)), 原始LR=hex(q(16)), 准备IO错误=hex(p[18]), 准备写入字节=p[19],
            准备close=hex(p[20]), 卡代次=p[21], 请求kbps=p[22], 请求大小=p[23], 请求arena=hex(q(24)),
            丢弃掩码=hex(p[26]), 到达掩码=hex(p[27]), arm=p[87], REC代次=p[118],
            事务阶段快照=list(p[119:125]), 原因掩码=hex(p[125]),
            原因=[v for k, v in REASONS.items() if p[125]&k], 当前runtime_epoch=p[126],
            shutdown=p[5], 模式=p[117], 诊断事务详情=details, 轻量记录=True,
            hand_back已返回=None))
    if len({r['会话'] for r in rows}) > 1:
        errors.append('多会话混合')
    return rows, errors

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('file', type=Path)
    p.add_argument('--json', action='store_true')
    args = p.parse_args()
    rows, errors = decode(args.file.read_bytes())
    print(json.dumps(dict(记录=rows, 警告=errors,
        限制='事件15不证明本次CLOSE返回。R/S/M正证据请用video_decoder.py；缺码不证明未到达。'),
        ensure_ascii=False, indent=2))
