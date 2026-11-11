# v36：本机实验室输入与离线复现

2026-11-11 已在 Ubuntu-26.04 找到原实验室目录、clang 适配器、MAIN 输入、
匹配的参考清单、各阶段产物与最终封装记录。`RELEASE-v36.md` 记录的是此前
源码交付时的状态；本文补充后续找到的证据与可移植入口，不把旧报告改写成新验证。

## 在其他机器上复现

`Ubuntu-26.04` 和下表中的 `$HOME` 路径只记录原实验室，既不是发行版要求，也不是克隆后会出现的文件。目标机器可以使用现有的 `Ubuntu`；无需安装同名发行版或重建原目录。

公开仓库不携带这些私有 MAIN。离线源码检查不需要它们；ELF 链接及完整 MAIN 链需要单独迁移输入。迁移包 `v36-private-inputs.zip` 的布局为：

```text
v36-private-inputs/
  core-baseline.main
  merged-base.main
  reference.main
  bootguard.main
  Z_8_0406.bin.v36
  manifests/base.json
  manifests/reference.json
  SHA256.json
  verify_inputs.py
  README.md
```

该包单独保存在原环境并交付，不是 GitHub 下载附件。复制到目标机器后解压，先在包目录运行 `python3 verify_inputs.py`，全部通过再继续。`bootguard.main` 和完整 `.v36` 是输出对照；实际编译输入是核心底座、merged base、reference 及匹配清单。

```sh
export INPUTS=/absolute/path/to/v36-private-inputs
python3 "$INPUTS/verify_inputs.py"
```

下方命令使用该变量。清单已随迁移包提供时可跳过 materialize 步骤，将其 `manifests/reference.json`、`manifests/base.json` 直接传给 link 和 chain。若只有源码而没有这些输入，link 之后仍无法执行；发行版名称或路径改名不能补回文件。

## 环境与源码检查

在本目录创建自己的 Python 环境，安装 `pyelftools`、`capstone`、`keystone-engine`。
需要 clang 21、ld.lld、llvm-nm、llvm-objcopy；bootguard 配方还使用 llvm-objdump。
这些程序须位于 PATH。Python 脚本使用当前解释器，不依赖私有 `.recorder-venv`。

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install pyelftools capstone keystone-engine
python -B tools-v36/compile.py
python -B tools-v36/layout.py
python -B tools-v36/audit.py
```

预期为 18 个 Live 和 10 个 PB 单元零诊断；runtime 大小 23184，close lock
偏移 18908 / 0x49dc，`live_w5.rmw` 偏移 1053592 / 0x101398，UiState 大小
37920，独占指令 194 条。结果写入本地 `audit-v36/`。

## 已找到的私有输入

|用途|实验室路径（相对 `$HOME`）|SHA-256|
|---|---|---|
|原始核心底座|`fxfix/base-c770f340.main`|`c770f34014849b2c42f20b9258a32b8959253cfb296e48f83417572781e00864`|
|ELF 链接参考|`.local/share/nikon-z8-qemu/live-audio/feat-rec/bg34.main`|`c7c6dbeea8a94064a10d47a7b9022ab26e4aa6b12ba5fb63b1ad85abac4fe4d4`|
|完整链输入|`nfp-rc2/newbase/newbase.main`|`54f25ad5d58d2c1f26498db7771d33816616e5494eb404f784cf8fe640d77c0e`|

这些文件及含原始字节的清单按现有发布约定保留本地。`receipts-v36/` 保存实际
文件/区间哈希、符号地址和历史元数据；不包含 `expected` / `replacement` 字节数组。
`materialize_manifest.py` 从本地文件重建私有清单，逐区间核对哈希。原始分支
guard 用目标地址表达，其余原始 guard 从已固定哈希的核心底座读取。

```sh
mkdir -p private-v36
python -B tools-v36/materialize_manifest.py \
  --recipe receipts-v36/reference-manifest-recipe.json \
  --image "$INPUTS/reference.main" \
  --baseline "$INPUTS/core-baseline.main" \
  --output private-v36/reference-manifest.json
python -B tools-v36/link.py \
  --reference-main "$INPUTS/reference.main" \
  --reference-manifest private-v36/reference-manifest.json
```

这一步只链接 ELF。也可通过 `V36_REFERENCE_MAIN`、`V36_REFERENCE_MANIFEST`
设置输入。需要 Zig fallback 时，将 `zig` 放入 PATH。

## 完整 MAIN 链

```sh
python -B tools-v36/materialize_manifest.py \
  --recipe receipts-v36/base-manifest-recipe.json \
  --image "$INPUTS/merged-base.main" \
  --baseline "$INPUTS/core-baseline.main" \
  --output private-v36/base-manifest.json
python -B tools-v36/chain.py \
  --base "$INPUTS/merged-base.main" \
  --manifest private-v36/base-manifest.json \
  --output-dir private-v36/chain
```

输出目录必须不存在。入口依次调用本仓库的 `relive_w5.py`、
`build-tools/pb/relive_pb.py`、`build-tools/usb_default_on.py`、
`build-tools/gui_pool_fix.py`。通过同目录 `compiler/clang` 对两个 PB UI 单元
使用 `-Oz`，并检查各阶段的完整哈希、Live payload 对 helper 区的边界，以及
最终 MAIN 的累计清单和范围外字节。无需复制实验室 `pbin/` 布局或运行会删除
旧目录的历史 `chain.sh`。

注意原 `newbase/changed-ranges.json` 的顶层输出哈希和两个 hook replacement
未随 rebase 更新。重建器保留历史字段，按实际输入生成新清单；不能把历史头部
直接当作输入身份校验。实验室当前的 pool 脚本已生成累计清单，旧源码交付说明
中“pool 没有清单”不适用于此次找到的产物。

链末 pool MAIN 的 SHA-256 应为：

```text
98638cf72deb1980fb7b3cdf2b82501f112303dad0ba9beb9a6757517b790544
```

最后的 bootguard 是独立步骤，源配方保存在 `tools-v36/bootguard_patch.py`：

```sh
python -B tools-v36/bootguard_patch.py \
  private-v36/chain/pool/pool.main private-v36/bg40.main
```

其 MAIN 预期为 `fecbe07e3a0ea2a9136b8e6a396e06b81d3d4e81eda7881c4b98ae20653be1f4`。
完整更新包的身份为 `04.06.p0`，SHA-256 为
`a6e1d92ef0c52fc4af066eb128460f8c01415e282ade12070e1c4bf5d70a6cde`。
MAIN 哈希不包含 EG 组件末尾的 CRC16，不能与 EG 组件哈希混用。

最终封装记录、阶段日志、哈希核验工具及此次复现结果位于
[z8-qemu 的 v36 证据目录](https://github.com/YCX81/z8-qemu/tree/main/experiments/20261111/v36)。
历史记录有 QEMU PASS，但明确没有 v36 真机回归。以上入口不连接相机、写卡或刷写。
