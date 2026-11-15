# 最终 v52E 镜像与打包路线独立审计

结论：**E 的源码、完整镜像、拼接、布局与内存内打包完整性审计通过**。这份报告不代替仍在进行的整机 QEMU 行为回归，也不代表真机验证。

- 最终 MAIN：`0008f196cf3e69edb12443e74f9b5466be762adc88c84a84c2192d1030437963`，81,807,709 B。
- 路径：`/path/to/user/fxfix/v52-root/v52e/q/main.main`；构建 `/path/to/user/v37build/v52e`。
- 本地 immutable `integration-e/work` 的全部 402 文件与 `integration/source-e.json` 一致；远端实际编译 work 的全部对应文件逐项一致。没有以 D 或可变 integration/work 代替 E。
- 独立重算 build ID：`0x25c3dd5b`，算法为最终 MAIN 该字段先置零，SHA-256 首 4B little-endian OR `0x01000000`。

## 镜像边界与全量重建

直接解析 ELF 的每个 alloc section，不只信任 build.json：每段均只读、非 NOBITS、不重叠，并在最终 MAIN 相同地址逐字节匹配。

| 部分 | 范围起点 | 实际字节 | 剩余空间 |
|---|---|---:|---:|
| Live primary | 401e2000 | 109920 | 672 |
| Live extra | 401fd218 | 2064 | 1496 |
| Playback | 40196000 | 36848 | 4112 |

`401fd000..401fd218` 的 536B size/meta 补丁区与 v51d 完全一致。extra 只复制显式 `.live_extra`，没有复制稀疏 ELF 间的洞。

我未执行 compose.py，而是独立重建：固定 v51d 基线→原始 rebuild 的逐字节差异（每个旧值先匹配）→ELF extra→两处快门 BL→独立计算的 EVF 40B→本 Agent 固定的 419 条 portrait 清单→重新计算 build ID。结果与最终 81 MB MAIN 完全一致。因此已有 v51 的独立 overlay 在这些明确改动之外保持原样。

另外独立从 `base-c770f340.main`，SHA `c770f34014849b2c42f20b9258a32b8959253cfb296e48f83417572781e00864`，按 aggregate manifest 的 154 段校验 expected、尺寸、非重叠并重放，得到同一完整 MAIN。Live/PB 当前符号及全部生成 hook 均已覆盖。PB 层按既有构建顺序覆盖的 10 处共享 hook 记录在 JSON；最终值逐项匹配 PB 最后层，不错误要求它同时等于中间 Live hook。

EVF 7处改动合计 40B，portrait 419 条完整记录/3518实际变化字节均和预先交付的严格清单匹配。原厂照片、横屏布局和旧 8357634 其他记录的保持性由该清单与前述全量重建共同覆盖。

v49 字体修复撤销保持：源码仍 `(void)live_font_fix;`，最终 ELF 没有链接该函数；UTF-16 `Live Photo: Preparing` 存在，Live 段内旧 Requested 不存在。内部、尾部和机器指令公开身份均为 `04.06.p0`。

## 工具与打包

已读并核对最终本地/远端 `build_v52.py`、`compose.py`、`pack52.py` SHA 相同，另审查 section splitter 和 EVF patch。

审查发现并由 root 修复的实质风险：旧 build_v52.py 的 `--stock` 可直接打包尚未 compose 的 raw v37.main，漏掉 extra/EVF/portrait。该参数及路径已删除，最终构建工具拒绝误传，README 明确先 compose 后 pack52。

未执行 pack52，也未生成交付目录；使用最终 E MAIN 在内存中调用 container replace，再以独立 verify.decode 解码验证：

- 原厂 3.11 输入 SHA 固定；MAIN 拼接 CRC-HQX 后 module CRC 为0。
- 新 EG 名称 `eg2070_0406p0.bi`；其余目录、模块、未选择字节原样。
- body CRC、module CRC、外部 SHA1 和长度均通过独立解码校验。
- 预测最终固件包 SHA：`3fb7db823d4029c149d245aaa12d265452a6913883e59dbe93f43e52cb62aacf`，99,051,117 B。正式 pack52 仍应核对这个值。
- v51 FENESTRA 来源实查仅3个常规文件（无软链接或特殊文件）：portrait.wlm、slotcard.wlm、STARTUP-Z8.0。脚本整体复制并逐文件读回比对；精确 SHA 在 JSON。
- test_movie 目标必须为 TEST_MOVIES 的直接子文件；新临时目录创建、既有最终目录/zip拒绝覆盖、目录rename和 ZIP CRC测试路径合理。本任务未触及相机、USB或物理卡。

## D 撤回与 E owner guard 的证据边界

D 曾把 VDI 第二个 DMA 分配记录误当原 arena；D 不交付。先前纯 guard 测试只能证明所给字段值的逻辑，并没有证明该字段的原厂语义。E 改为真正的 `a02d3d950`：原厂 `40c3e9a4` 建立结构基址 `a02d3d928`，`40c3e9f0..40c3ea04` 读取原 arena 并写入结构+0x28。旧 `a02d3d9d8` 是另一字段。

本次进一步直接执行 **最终 v52e 链接的 Live ELF**，而非另编的小替代实现：正确 d950=arena、旧 d9d8=arena+2 MiB；两个slot共54个owner例及12个early-close例，共66 PASS。旧DMA字段为0或等于arena也不影响合法owner；原arena、实例、状态、codec不符仍在首次硬件API前拒绝，busy/close lock正确退出。此测试不模拟完整 EOS/IRQ/EXIT；整机关闭时序仍由其他 lane 负责。

机器证据：`final-binary-e.json`、`final-binary-owner-e.json`、`final-binary-tools-sha256.json`。审计脚本为 `final-binary-audit.py` 与 `final-binary-owner-e.py`。
