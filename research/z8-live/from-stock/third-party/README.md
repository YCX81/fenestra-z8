# NikonFirmwarePatch toolkit dependencies

The two unchanged ZIP archives are the upstream public toolkit distributions:

| Archive | Bytes | SHA-256 |
|---|---:|---|
| `NikonFirmwarePatch-v0.3.0-r1-toolkit.zip` | 1,022,549 | `704dbb2e09abed4e641174810ad655a413a2eff4042845371499c896634591ed` |
| `NikonFirmwarePatch-v0.4.0-rc.2-toolkit.zip` | 8,046,605 | `9a00183b54ee80e02e89445a59f5806a1e738ea22e2400538221ca27c21d9b4c` |

Origins: [v0.3.0-r1](https://github.com/XinmiaoZhao/NikonFirmwarePatch/releases/tag/v0.3.0-r1),
[v0.4.0-rc.2](https://github.com/XinmiaoZhao/NikonFirmwarePatch/releases/tag/v0.4.0-rc.2).
Attribution, license texts, source and notices remain intact inside each archive.
NikonFirmwarePatch source and its authored compiled patch units are licensed
GPL-3.0-only. They are separate from the Fenestra CC BY-NC-SA adaptation and its
additional photography permission.

The archives contain no complete Nikon update package or extracted MAIN. Their
small `resources/*.bin` files are upstream-authored patch units with corresponding
C/assembly source, compiled metadata and source-hash checks in the same toolkit.
The reconstruction uses the toolkit's verified prebuilt mode for those units.

Bundling the original archives preserves the exact reviewed input even when an
upstream release attachment is unavailable. The builder verifies the complete
archive before importing or extracting its code. No firmware, private laboratory
MAIN, calibration data or camera media is bundled here.
