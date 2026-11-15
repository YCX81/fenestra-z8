"""build_v53.py with two extra BL sites for the one-MOV experiment."""
import sys,runpy
from pathlib import Path
src=Path('/path/to/user/fxfix/v53-root/build_v53.py').read_text()
a="pb_builder.write_text(pb_text.replace('0x401a0000','0x4019f020'))"
assert src.count(a)==1
src=src.replace(a,"""pb_text=pb_text.replace('0x401a0000','0x4019f020')
    site="(0x40618be0, 'pb_ui_poll_wrap', True),"
    assert pb_text.count(site)==1
    pb_text=pb_text.replace(site,site+" (0x4063b958,'pb_native_return',True), (0x402d9ba0,'pb_open_wrap',True), (0x4085fcc0,'pb_stat_wrap',True), (0x40711334,'pb_info_record',True),")
    pb_builder.write_text(pb_text)""")
src=src.replace("Path(__file__).resolve().with_name('split_live_payload_v53.py')","Path('/path/to/user/fxfix/v53-root/split_live_payload_v53.py')")
exec(compile(src,'build_onemov','exec'))
