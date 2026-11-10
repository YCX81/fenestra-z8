# Fenestra for Nikon Z8 C3.11

[中文功能与使用指南](docs/features-and-usage.zh-CN.md) · [Live source archive](research/z8-live/README.md) · [Core port history](port-z8/PORT-STATUS.md)

This is YCX81's Z8 C3.11 adaptation of [WonderLantern/Fenestra](https://github.com/WonderLantern/Fenestra), based on `67774849c9dd97cc7127066190528e245ee25650`. The default branch is `z8-c311`. The November Live development snapshot has been merged into this branch; its temporary progress branch has been removed.

## Features and status

| Area | What the repository provides | Status |
|---|---|---|
| Core runtime | Native Fenestra / WonderLantern menus, activation and shutdown guard, card module loading, SDK, hello and animated demo | The root core has recorded QEMU runs; physical-camera qualification remains incomplete |
| Host bridge | Z8 C3.11 bindings, hash-pinned AArch64 Linux agent and parameterless INFO protocol | File-backed software round trip recorded; camera USB/PTP and shell execution are not established capabilities |
| Live capture | On/Off menu, rolling Wave5 H.264 capture around a still, MOV/photo pairing, audio integration and file transactions | Separate research source, with multiple historical revisions; v36 is a candidate |
| Live playback and UI | LVP preview via REC, portrait handling, transparent LIVE badges and busy animation | Archived implementation; v36 includes the 110×44 assets, with final display/hardware regression pending |
| Wave5 hand-back | Cached runtime close lock and preserved bring-up hand-back behavior | v36 source/object audit recorded; earlier diagnostic-version hardware reports do not qualify v36 |
| C64 labels | A separate guarded descriptor-style patch for an already expanded C64 MAIN | Not a slot expansion tool; shared-page rendering needs verification |
| Diagnostics | REC recorders/lamp-code experiments, Wave5 lifecycle analysis, media/icon checks and build tools | Version-specific development tools, not interchangeable with the ordinary candidate |

The root `src/` and `host/` port and the later `research/z8-live/` tree are separate build routes. Merging the archive does not automatically integrate Live into the root builder. Historical Live records include hardware observations for particular diagnostic builds; the root core's older “QEMU only” status must not be generalized to every archived experiment. See the [Chinese guide](docs/features-and-usage.zh-CN.md) for the exact source, workflow and evidence boundaries.

There is no camera-installable, hardware-qualified Z8 release package here. Do not treat a generated MAIN or inherited ZR/Z9 release link as one. Upstream tools and target support for ZR C1.10 / Z9 C5.32 remain in the source tree, but those artifacts do not support Z8.

## Build the core and a module

Use Python 3.11+, LLVM (`clang`, `ld.lld`, `llvm-objcopy`, `llvm-nm`) and a static-capable AArch64 Linux GCC for the INFO agent. Supply your own identity-matched original Z8 MAIN; firmware and key material are not distributed.

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r requirements.txt
python host/build_z8_core.py \
  --firmware /absolute/path/to/z8-c311-original-main.bin \
  --output-dir build/z8-core-local
python host/build_sdk_module.py examples/hello/module.c \
  --id hello --target z8-c311 --output-dir build/hello-z8-local
```

Use fresh output directories. The core builder's `--agent-cc` defaults to `aarch64-linux-gnu-gcc`. Its MAIN output is an emulation candidate, not an update container. The generic `fenestra.py make-patch` route does not generate a Z8 release.

Place a compatible `.wlm` in `FENESTRA/` on the matching experiment's virtual card, activate through Setup → Fenestra, then open its page through WonderLantern. The port's launch helpers require the separate research environment described in the [port history](port-z8/PORT-STATUS.md). The [QEMU guide](https://github.com/YCX81/z8-qemu/blob/main/docs/features-and-usage.zh-CN.md) covers the companion emulator's baseline setup.

## Live development workflow

Start with the [functional and usage guide](docs/features-and-usage.zh-CN.md), then the [archive map](research/z8-live/README.md) and [v36 candidate notes](research/z8-live/build-tools/livediag/feat-release/RELEASE-v36.md). The guide explains the Live menu, REC preview, file locations, C64 patch and offline compilation commands, and identifies the private inputs and missing laboratory helpers.

Historical source trees, failed experiments and local artifact references are retained for traceability. Source presence is not a claim of end-to-end acceptance, iPhone import compatibility, stock MOV playback compatibility or complete recording-mode regression.

## Documentation and validation

- [Chinese feature and usage guide](docs/features-and-usage.zh-CN.md)
- [Getting started and inherited target workflows](docs/getting-started.md)
- [Architecture](docs/architecture.md), [platform porting](docs/porting.md), [SDK API](docs/api-reference.md)
- [Module development](docs/module-development.md), [module format](docs/module-format.md)
- [Host debugging and INFO bridge](docs/debugging.md)
- [Validation](docs/validation.md), [source publication](docs/publication.md)

Run `python -B verify_source.py` to check the source inventory. This checks bytes, not camera safety or firmware behavior. The current documentation update does not add QEMU, USB or hardware acceptance results.

## License

[CC BY-NC-SA 4.0](LICENSE), with the [commercial photography permission](EXCEPTIONS.md) and [Z8 adaptation scope](LICENSE-Z8.md). Paid photography and filmmaking are permitted under that additional permission; selling covered software or paid flashing services is not authorized. The NonCommercial restriction makes this source-available rather than OSI open source. Separately licensed dependencies retain their own terms; original firmware and private inputs are outside this grant.
