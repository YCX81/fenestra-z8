"""Public source adapted from the pinned research implementation; regular files only."""
from z8prores.package import require
from z8prores.a64 import Block as _Block, _imm
"The ProRes menu line's minimal AArch64 encoder plus the forms the N-RAW four-tier patch needs.\n\nEvery emitted word is disassembled again with Capstone (`Block.listing`), so a wrong encoding shows up as text that\ndiffers from what was asked for.  Offline only.\n"
COND = {'eq': 0, 'ne': 1, 'hs': 2, 'lo': 3, 'mi': 4, 'pl': 5, 'hi': 8, 'ls': 9, 'ge': 10, 'lt': 11, 'gt': 12, 'le': 13}

class Block(_Block):

    def ldp_x(self, rt, rt2, rn, offset):
        """ldp xT,xT2,[xN,#offset]"""
        self._w(2839543808 | _imm(offset // 8, 7, True) << 15 | rt2 << 10 | rn << 5 | rt)

    def stp_x(self, rt, rt2, rn, offset):
        """stp xT,xT2,[xN,#offset]"""
        self._w(2835349504 | _imm(offset // 8, 7, True) << 15 | rt2 << 10 | rn << 5 | rt)

    def sub_sp(self, value):
        self._w(3506438144 | _imm(value, 12) << 10 | 31 << 5 | 31)

    def add_sp(self, value):
        self._w(2432696320 | _imm(value, 12) << 10 | 31 << 5 | 31)

    def sub_xi(self, rd, rn, value):
        self._w(3506438144 | _imm(value, 12) << 10 | rn << 5 | rd)

    def add_wi(self, rd, rn, value):
        self._w(285212672 | _imm(value, 12) << 10 | rn << 5 | rd)

    def sub_wi(self, rd, rn, value):
        self._w(1358954496 | _imm(value, 12) << 10 | rn << 5 | rd)

    def cmp_x(self, rn, rm):
        self._w(3942645791 | rm << 16 | rn << 5)

    def rev_w(self, rd, rn):
        self._w(1522534400 | rn << 5 | rd)

    def rev16_w(self, rd, rn):
        self._w(1522533376 | rn << 5 | rd)

    def mul_w(self, rd, rn, rm):
        self._w(453016576 | rm << 16 | rn << 5 | rd)

    def umull(self, rd, rn, rm):
        """umull xD,wN,wM"""
        self._w(2610985984 | rm << 16 | rn << 5 | rd)

    def lsl_xi(self, rd, rn, shift):
        """lsl xD,xN,#shift (ubfm alias)"""
        self._w(3544186880 | (64 - shift) % 64 << 16 | 63 - shift << 10 | rn << 5 | rd)

    def cset_w(self, rd, cond):
        """cset wD,cond (csinc wD,wzr,wzr,!cond)"""
        self._w(446629856 | (COND[cond] ^ 1) << 12 | rd)

    def cbnz_x(self, rt, target):
        self._rel(lambda pc: 3036676096 | _imm(self._target(target) - pc >> 2, 19, True) << 5 | rt)

    def b_cond(self, cond, target):
        code = COND[cond]
        self._rel(lambda pc: 1409286144 | _imm(self._target(target) - pc >> 2, 19, True) << 5 | code)

    def word_at(self, va):
        """Index of the item emitted for address va (for stating what a label holds)."""
        return (va - self.va) // 4

    def and_w31(self, rd, rn):
        self._w(301993984 | rn << 5 | rd)

    def lsr_xi(self, rd, rn, shift):
        self._w(3544251392 | _imm(shift, 6) << 16 | rn << 5 | rd)

    def lsr_wi(self, rd, rn, shift):
        self._w(1392540672 | _imm(shift, 5) << 16 | rn << 5 | rd)
