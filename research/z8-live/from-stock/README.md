# 从原厂 C 3.11 重建 v36

本入口以自行取得的完整 `Z_8_0311.bin` 为唯一固件输入，生成与历史 v36 相同的
MAIN 和完整更新容器。无需 `base-c770f340.main`、`newbase.main`、`bg34.main`、
旧实验室清单或迁移包。输出由固定 SHA-256 验证；本入口不连接相机或写卡。

## 准备

克隆或更新两个公开仓库，分别使用默认分支：

```sh
git clone --branch z8-c311 https://github.com/YCX81/fenestra-z8.git
git clone --branch main https://github.com/YCX81/z8-qemu.git
cd fenestra-z8
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r research/z8-live/from-stock/requirements.txt
```

在 Linux 或现有 WSL `Ubuntu` 中执行即可，无需名为 `Ubuntu-26.04` 的发行版。
准备 clang 18、clang 21 和 LLVM 21 的 `ld.lld`、`llvm-nm`、`llvm-objcopy`、
`llvm-objdump`。已验证的编译器分别为 Ubuntu clang 18.1.8 和 21.1.8。
历史核心及旧录像 Live 用 clang 18，v36 Live/回放和 bootguard 用 clang 21。
不同版本只有在全部固定哈希仍一致时才算复现成功。

原厂输入必须为 99,051,117 字节，SHA-256：

```text
d67413c2ede6b9ce8887175f20700bd9fbad2a973a4820968c45503d15d80d96
```

## 一条构建命令

在 Fenestra 仓库根目录运行，输出目录必须不存在：

```sh
python -B research/z8-live/from-stock/build.py \
  --stock /absolute/path/to/Z_8_0311.bin \
  --qemu-root /absolute/path/to/z8-qemu \
  --output-dir /absolute/path/to/v36-rebuilt
```

需要指定工具链时，加 `--core-clang /path/to/clang-18`、
`--live-clang /path/to/clang-21`、`--llvm-bin /path/to/llvm-21/bin`。
只构建 MAIN 可加 `--main-only`。

公开 NFP 工具包以原始 ZIP 随仓库保存在 `third-party/`，包含对应源码、预编译单元及许可；正常构建无需再访问 Release 下载地址。工具包会在输出目录中解压，整包尺寸及 SHA-256 均固定：

|版本|工具包 SHA-256|
|---|---|
|[v0.3.0-r1](https://github.com/XinmiaoZhao/NikonFirmwarePatch/releases/tag/v0.3.0-r1)|`704dbb2e09abed4e641174810ad655a413a2eff4042845371499c896634591ed`|
|[v0.4.0-rc.2](https://github.com/XinmiaoZhao/NikonFirmwarePatch/releases/tag/v0.4.0-rc.2)|`9a00183b54ee80e02e89445a59f5806a1e738ea22e2400538221ca27c21d9b4c`|

也可自行从这两个公开 Release 取得完整 toolkit ZIP，再通过
`--nfp-r1-toolkit /path/to/r1.zip --nfp-rc2-toolkit /path/to/rc2.zip` 提供，仍执行相同校验。

本入口使用 NFP 官方工具包随附、与其源码配套的自编译补丁单元。这些是公开依赖，
不是私有 MAIN。它不声称可用任意 Linux clang 逐个重编 NFP 的第三方补丁并获得
相同字节；NFP 自身的源码编译要求仍由其发布配方规定。Fenestra、C64、布局、旧 Live
和 v36 的代码则由本仓库及配套源码在构建时生成。

## 生成过程

1. 验证原厂完整包，校验外层封装和组件，提取固定身份的原始 MAIN。
2. 从 `baseline-core`、`core-overlay` 和原厂指令生成历史核心；恢复 C64 卡槽服务、
   原桥接接口及对应结构布局。clang 18 的输出经完整核心哈希核对。
3. 核心中的 32 字节运行身份恢复为历史值；当前源码及编译器产生的真实身份另存
   `reconstruction.json`。只有完整核心的其余字节完全一致时才接受此操作。
4. 组合公开 NFP r1、C64、竖屏布局和旧录像 Live，重新生成 `c770f340…` 底座。
5. 替换为 NFP rc.2 的 `framing + video-quality + photo-tools`，保留 C64 的加载边界。
   新生成的 seed 留空两层 Live payload 区，并从当前指令与核心符号生成清单。
6. 直接编译 v36 Live 和回放，依次生成 USB、GUI pool 和 bootguard 修正，核对 ELF
   约束、payload 保留区及每个最终 MAIN 的固定哈希。
7. 将 MAIN 和 CRC16 放回原厂容器，修复封装校验，并核对整个更新文件。

旧 `54f25ad5…` merged MAIN 和 `c7c6dbee…` 链接参考不再是这个流程的输入。
原链为了保留旧 Live 版本而引入了它们；v36 会完全覆盖这些旧 payload 和挂点。
新流程为当前源码生成清单，直接完成链接和合成，最终输出仍逐字节一致。

## 输出与核验

|文件|SHA-256|
|---|---|
|`core-baseline.main`|`c770f34014849b2c42f20b9258a32b8959253cfb296e48f83417572781e00864`|
|`pool/pool.main`|`98638cf72deb1980fb7b3cdf2b82501f112303dad0ba9beb9a6757517b790544`|
|`v36.main`|`fecbe07e3a0ea2a9136b8e6a396e06b81d3d4e81eda7881c4b98ae20653be1f4`|
|`Z_8_0406.bin`|`a6e1d92ef0c52fc4af066eb128460f8c01415e282ade12070e1c4bf5d70a6cde`|

`reconstruction.json` 保存编译器、公开依赖、当前核心源码身份和各阶段哈希；
`logs/` 保存命令输出，失败时先看对应步骤的日志。源码准备和生成过程均在新输出
目录中进行，不修改原厂文件或源码仓库。完整包与 MAIN、含指令字节的本地清单和
构建日志均留在本地，不应加入公开源码提交。

历史 v36 没有完成真机回归；逐字节重建不会改变这项状态。

完整独立检查记录见配套仓库的 [from-stock-reconstruction.json](https://github.com/YCX81/z8-qemu/blob/main/experiments/20261111/v36/from-stock-reconstruction.json)。该次运行禁止 Python 读取旧实验室目录、卡备份和迁移包，并生成了相同的完整更新容器。
