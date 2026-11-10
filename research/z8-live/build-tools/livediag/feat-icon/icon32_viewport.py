#!/usr/bin/env python3
"""Decode a pinned Z8 Live overlay from existing raw RAM dumps or a supplied reader.

No USB connection is created. A caller with an open, authorized read-only
transport can pass its read(address, size) callback to sample().
"""
import argparse
import json
import struct
from pathlib import Path

RAM = 0xa00000000
END = 0xa40000000
# The display and gate offsets are identical across these PbRuntime sizes.
PB_LAYOUT_BYTES = (8240, 8344, 8352, 8400)


def overlay_descriptor_address(device, kind, index):
    """Type 1 branch of the pinned MAIN's 0x4033d8cc descriptor lookup."""
    if kind != 1 or device not in (0, 1, 2) or not 0 <= index <= 4:
        return None
    base = 0xa02f35f98
    if device == 0:
        return base
    return base + (0xb0 if device == 1 else 0xad8) + index * 0x208


def require(ok, reason):
    if not ok:
        raise ValueError(reason)


def sample(read, mode):
    """Sample once. Reject a disabled or stale overlay before using its geometry."""
    require(mode in ('shoot', 'pb'), 'choose shoot or pb')
    def fields(addr, fmt):
        fmt = '<'+fmt
        size = struct.calcsize(fmt)
        require(RAM <= addr <= END-size, f'address outside target RAM: {addr:#x}')
        data = read(addr, size)
        require(len(data) == size, f'short read at {addr:#x} (need {size} bytes)')
        return struct.unpack(fmt, data)
    def ptr(addr):
        value, = fields(addr, 'Q')
        require(RAM <= value < END and value % 8 == 0, f'invalid pointer at {addr:#x}: {value:#x}')
        return value

    root = ptr(0xa032b50d8)
    core = ptr(root+0xf8f0)
    magic, installed = fields(core+64, 'II')
    require(magic == 0x465a394d and installed and fields(core+32,'H')==(46,), 'core identity/row count mismatch')
    reg = ptr(core+2528)
    require(fields(reg,'II') == (0x47525846, 64), 'registry magic/size mismatch')
    ui = ptr(reg+8)
    require(fields(ui,'I')[0] in (0x4c505549, 0x4c50554a, 0x4c50554b), 'unknown shooting UI layout')
    require(fields(ui+24,'I') == (0,), 'shooting UI is shutting down')
    usb, usb_events = fields(ui+0x4828, 'II')
    view = ptr(0xa032280c8)
    view_id, = fields(view+0x54,'I')
    view_state, = fields(view+0xd0,'I')
    expected_id = 3 if mode=='shoot' else 2
    require((view_id,view_state)==(expected_id,1), f'current view is {view_id}/{view_state}, expected {expected_id}/1')
    selector, = fields(0xa03bd062c,'I')
    attitude, = fields(0xa03bc8004,'I')
    ui_direction, = fields(0xa03228244,'I')
    suppress_mask, = fields(0xa03228308,'I')
    if mode == 'shoot':
        require(selector==0, 'PHOTO selector is not active')
        owner, display, gate_addr = ui, ui+12072, ui+18144
    else:
        owner = ptr(reg+16)
        require(fields(owner,'I')==(0x50425549,) and fields(owner+4,'I')[0] in PB_LAYOUT_BYTES, 'unknown playback layout')
        require(fields(owner+16,'I')==(0,), 'playback is shutting down')
        display, gate_addr = owner+1856, owner+32
    require(fields(display+224,'I')==(1,), 'display is detached')
    gate, = fields(gate_addr, 'Q')
    require(ptr(display+6040)==gate_addr and gate&1, 'draw gate closed or owner mismatch')
    require(fields(display+6024,'I')==(1,) and ptr(display+6016)==view, 'global overlay belongs to another view')
    surface = ptr(display+200)
    target = fields(display+6056,'III')
    require(target==fields(surface+0x108,'III'), 'native target changed')
    w,h = fields(display+216,'II')
    rect = fields(display+6032,'4H')
    left,top,right,bottom = rect
    require(0<=left<right<=w<=4096 and 0<=top<bottom<=h<=4096, 'invalid backing or viewport')
    count, = fields(display+228,'I')
    error, = fields(display+240,'I')
    require(count and not error, 'no successful frame or pending draw error')
    desc = overlay_descriptor_address(*target)
    plane = None
    if desc is not None:
        size = fields(desc+8,'HH')
        backing, = fields(desc+0x28,'Q')
        fmt, = fields(desc+0x3c,'I')
        callback, = fields(0xa02f35f50,'Q')
        plane = dict(address=hex(desc), dimensions=size, backing=hex(backing),
                     format=fmt, notification=hex(callback))
    return dict(mode=mode,root=hex(root),core=hex(core),registry=hex(reg),ui=hex(ui),owner=hex(owner),
                display=hex(display),view=hex(view),view_id=view_id,view_state=view_state,
                surface=hex(surface),target=target,overlay_plane=plane,selector=selector,attitude=attitude,
                ui_direction=ui_direction,suppress_mask=suppress_mask,
                usb=usb,usb_events=usb_events,backing=(w,h),viewport=rect,frames=count,gate=gate)


def compare(first, second):
    # Epoch and frame count normally advance during animation. Both captures
    # have already passed current-view, native-surface and enable-bit checks.
    changed = [k for k in first if k not in ('frames','gate') and first[k]!=second[k]]
    require(not changed, 'different view/geometry between samples: '+', '.join(changed))


def from_dump(path, base, mode):
    with path.open('rb') as stream:
        def read(addr, size):
            require(addr>=base, f'address {addr:#x} below dump base')
            stream.seek(addr-base)
            return stream.read(size)
        return sample(read,mode)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode',choices=('shoot','pb'))
    parser.add_argument('dump',type=Path,help='first raw contiguous RAM capture')
    parser.add_argument('--second',type=Path,help='independent second capture of the same view')
    parser.add_argument('--base',type=lambda s:int(s,0),default=RAM)
    args=parser.parse_args()
    try:
        first=from_dump(args.dump,args.base,args.mode)
        rows=[first]
        if args.second:
            require(not args.dump.samefile(args.second), 'two independent captures required')
            second=from_dump(args.second,args.base,args.mode)
            compare(first,second)
            rows.append(second)
    except (OSError,ValueError) as error:
        parser.exit(1,f'Viewport unavailable: {error}\n')
    print(json.dumps(dict(samples=rows,consistent_pair=len(rows)==2),indent=2))


if __name__=='__main__':
    main()
