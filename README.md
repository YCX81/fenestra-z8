# Fenestra for Nikon Z8 C3.11

[中文功能与使用指南](docs/features-and-usage.zh-CN.md) · [Live source archive](research/z8-live/README.md) · [Core port history](port-z8/PORT-STATUS.md)

This is YCX81's Z8 C3.11 adaptation of [WonderLantern/Fenestra](https://github.com/WonderLantern/Fenestra), based on `67774849c9dd97cc7127066190528e245ee25650`. The default branch is `z8-c311`. The November Live development snapshot has been merged into this branch; its temporary progress branch has been removed.

## Features and status

| Area | What the repository provides | Status |
|---|---|---|
| Core runtime | Native Fenestra / WonderLantern menus, activation and shutdown guard, card module loading, SDK, hello and animated demo | The root core has recorded QEMU runs; physical-camera qualification remains incomplete |
| Host bridge | Z8 C3.11 bindings, hash-pinned AArch64 Linux agent and parameterless INFO protocol | File-backed software round trip recorded; camera USB/PTP and shell execution are not established capabilities |
| Live capture | On/Off menu, rolling Wave5 H.264 capture around a still, MOV/photo pairing, audio integration and file transactions | Separate research source, with multiple historical revisions; v62 is the published checkpoint, with user-confirmed validation |
| Live playback and UI | Native MOV playback via REC in v54–v62, portrait handling, LIVE badges and busy animation | v62 includes RAW/JPEG peer playback, Log text and shutter sound fixes; user validation recorded |
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

Start with the [functional and usage guide](docs/features-and-usage.zh-CN.md), then the [archive map](research/z8-live/README.md) and [v62 checkpoint notes](research/z8-live/build-tools/livediag/feat-onemov/RELEASE-v62.md). The guide explains the Live menu, REC preview, file locations, C64 patch and offline compilation commands, and identifies the private inputs. The [stock-to-v36 workflow](research/z8-live/from-stock/README.md) builds the matching MAIN and update container from an original C 3.11 package, both repositories and bundled public NFP dependencies; no private intermediate MAIN is required.

Historical source trees, failed experiments and local artifact references are retained for traceability. Source presence is not a claim of end-to-end acceptance, iPhone import compatibility, stock MOV playback compatibility or complete recording-mode regression.

## Documentation and validation

- [Chinese feature and usage guide](docs/features-and-usage.zh-CN.md)
- [Getting started and inherited target workflows](docs/getting-started.md)
- [Architecture](docs/architecture.md), [platform porting](docs/porting.md), [SDK API](docs/api-reference.md)
- [Module development](docs/module-development.md), [module format](docs/module-format.md)
- [Host debugging and INFO bridge](docs/debugging.md)
- [Validation](docs/validation.md), [source publication](docs/publication.md)

Run `python -B verify_source.py` to check the source inventory. This checks bytes, not camera safety or firmware behavior. This checkpoint publishes existing v62 source and test receipts with user validation; no repeated build or runtime tests; historical QEMU and hardware observations remain version-specific.

## Worktree cleanup / 工作树清理（2026-11-15）

本次仅核对源码归档、Git 历史、构建路径引用和运行进程，未重新编译、运行测试或启动 QEMU。

| 工作树或源码副本 | 处理 | 依据 |
|---|---|---|
| Mac `z8-portrait-menu` | 完整备份及 Git bundle 后删除；分支保留 | 布局和直方图源码从 V50 起集成并沿用到 V62；原 `8357634` 分支现已合入 QEMU `main`，不再需要独立工作树 |
| WSL `v37hevc-src`、`v37src`、`v39src`、`v40src` | 含 Git 历史及本地改动的备份完成后删除 | 共同 V37 基线已合并；所有有效本地改动均能逐文件匹配当前源码归档，V62 构建链及运行进程不再引用这些目录 |
| WSL `v41src`、`v43src`–`v45src`、`v46src`、`v46rsrc`、`v47src`–`v50src` | 保留 | 含尚未逐字归档的历史实现；不能仅按旧版本号认定其内容都可删除 |
| WSL `v51src` | 保留 | V52/V53 构建器仍直接引用其中的构建工具，V62 延用这条前置链 |
| WSL QEMU `merged/tree`、`z8-he-history-20261017` | 保留 | 前者有 11 个已跟踪文件改动，后者是独立 HE/RAW 研究；均未证明被 V62 覆盖 |
| 其他仓库主工作目录 | 保留 | 上游工具、密钥工具、其他机型和独立研究仓库不属于可删除的 V62 重复工作树 |

本次 Mac 备份位于工作区 `worktree-backups/portrait-before-removal-20261115/`，含完整工作文件与 `portrait-history.bundle`。WSL 备份位于 `$HOME/worktree-backups/v62-covered-sources-20261115/`，每个目录分别保存 `.tar.gz`、文件数及 SHA256 收据。删除前均核对归档内容；恢复时解包到原路径，portrait 分支也可重新创建工作树。备份包含未提交内容，保留本地，不上传 GitHub。

此前清理的 Mac `fenestra-z8-v37` 和 WSL `v37cs` 的备份继续保留；当前 V62 源码、交付包和实验室构建输入未因本次清理而移动。

## License

[CC BY-NC-SA 4.0](LICENSE), with the [commercial photography permission](EXCEPTIONS.md) and [Z8 adaptation scope](LICENSE-Z8.md). Paid photography and filmmaking are permitted under that additional permission; selling covered software or paid flashing services is not authorized. The NonCommercial restriction makes this source-available rather than OSI open source. Separately licensed dependencies retain their own terms; original firmware and private inputs are outside this grant.
