"""Source-only rejection checks for the stock reconstruction entry."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('from_stock_build',
                                             ROOT / 'research/z8-live/from-stock/build.py')
BUILD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILD)


class FromStockAdmissionTests(unittest.TestCase):
    def test_toolkit_identity_is_checked_before_extraction(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            supplied = root / 'wrong.zip'
            supplied.write_bytes(b'not the pinned public toolkit')
            with self.assertRaisesRegex(ValueError, 'SHA-256 differs'):
                BUILD.toolkit('r1', supplied, root)
            self.assertFalse((root / 'r1').exists())

    def test_independent_source_layers_reject_overlapping_ownership(self):
        original = bytes(8)
        image = bytearray(original)
        first = bytearray(original)
        first[3] = 1
        BUILD.combine(original, image, first, 'first owner')
        conflicting = bytearray(original)
        conflicting[3] = 2
        with self.assertRaisesRegex(ValueError, 'overlaps another layer'):
            BUILD.combine(original, image, conflicting, 'second owner')
        self.assertEqual(image[3], 1)


if __name__ == '__main__':
    unittest.main()
