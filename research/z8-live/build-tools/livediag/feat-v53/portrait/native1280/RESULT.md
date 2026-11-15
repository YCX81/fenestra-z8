# 1280×852 原厂完整播放调查（冻结结果）

**结论：几何初始化和前 16 帧显示路径已成立，当前 VideoToolbox 来源的 90 帧样片没有完整播放通过。未修改生产固件、writer、原 fixture、共享 QEMU 或物理卡。**

## 输入与已排除项

实际生产 writer fixture `base.MOV` SHA-256：`3091e8dbeb8265f1dd49564edbdb8ee95ad94c8ad0db5e9316af76addae4d5f8`，1280×852、90 个 HEVC AU、无音轨、无 LVJ1。

- 实际 MOV 每帧已经有 AUD：6 个 IRAP 为 NAL `[35,39,20]`，84 个 P 帧为 `[35,1]`。原 VideoToolbox 素材没有 AUD，不适用于此生产 writer 输出。
- 表为 `stsc=(1,16,1),(6,10,1)`、6 个连续 chunk、sync=1/17/33/49/65/81；原厂已读到 total=90、GOP=16、1280×852。不能将它解释成一个仅有 16 帧的文件。
- 原样 rdw `nikon-wave5-decode.h` + libavcodec host harness 解出全部 90 帧。每 GOP 加 4096 个零的对照也解出 90 帧。
- 1280c 的实际 RAM 输入捕获覆盖 AU15 至 AU79，共 3,407,122 字节；全部逐字节等于原 MOV 对应 65 个 AU 加每 GOP 4096 个零。第二个 IDR 以及后续四个 GOP 已正确送入 ring，没有发现读卡或码流截断。见 `actual-ring-comparison.json`。
- VPS/VUI 的 timing flag 原为 0，实际 helper 报 fps=0/0。root 生成只增加 17 字节 timing 参数集的独立 VUI fixture（`3ca5c42a…`），90 个 MOV sample 与 decoded pixels 保持相同；独立 helper 报 fps=30000/1001、90/90 帧。但原厂整机仍只解 17 次，因此正确 VUI 帧率不足以解决此问题，不能将 fps=0 判为原因。

## 整机对照

| 路线 | 实际结果 | 边界 |
|---|---|---|
| 原 N4 /1280b，原 rdw QEMU，40 s | 首图 1 + 播放 16；随后 stop/flush；收尾出现 GPIO unknown | GPIO 在停止之后，不能代替此前停播原因 |
| 1280c，日志 observer，30 s | 同样 1+16、exit0、无 picture false 分支 | 30 s 结束过早，exit0 不等于完整播放 |
| 1280d，仅 REC 脉冲缩到 26.0–26.05 s | 同样 1+16 | 不能用此结果给 REC latch 修复背书 |
| root final-F 标准原厂 Play/OK，无 N2 bridge | 同样 1+16、235 checks、无 unknown | 已排除 N2 bridge 是充分原因 |
| root final-F 标准 Play/OK，VUI 单变量 | 同样 1+16、235 checks、无 unknown | VUI FPS 单独修正不足 |
| 1280g，final-F + observer2，标准 Play/OK，40 s | 同样 1+16、exit0；完整 queue/upper stop 证据 | **full_playback=false** |

另有独立无音轨正例：`native-rec/evidence/probe-n3/run-small-summary.json` 已核，真实 Live 100 帧、native_audio_tracks=0，实际 101 次 picture（首图+100）、237 checks、photo→movie→photo。因此不能泛称无音轨不能完整播放。

## 最终 1280g 闭合的停止链

final-F MAIN `b0cd7c40…` 未改；observer2 可执行文件 `69fdc3b5…`。原 rdw binary 为 `a88a921c…`。

1. 播放阶段实际提交并接受 **16** 个 picture command；16 个均成功，最后 poc=15。没有第 17 个 accepted command，也没有 picture false 分支。
2. 最后剩余显示引用为 flags=1；另外两个槽已释放，command_count=0。不是已有命令等待空闲显示槽。
3. FLUSH 在虚拟时间 **30.091271660 s**：outputs=16、count=0、timer=0、frames=0、result=0。
4. 唯一非零 upper stop 调用：`403f6dc0(x0=1,w1=2)`，LR=`406678d4`。
5. 对最终 F 的原厂指令核对：`40667804` 先调用 `403f6d1c`，将返回值交给 `403f6dc0`；前者检测并清除 `a032cd300` 后返回 2，后者进入 `403f6e88/6f10` 停止/清理。
6. 静态上，callback `403f7038` 收到 `0x04090105` 会置 `a032cd300=1`；映射器 `40719000` 将输入 bit `0x08000000` 映成该消息。`[4164b558]` 是常量 1，**不是动态错误编号**。

第 5–6 步的函数区间已确认 G/F 字节相同，并保存完整反汇编。此次未直接探测 `403f7038/40719000` 的事件输入，故不能把静态映射冒充已采集的低层错误现场。真正导致 bit08000000 的异常仍未定位。

证据：`observer2/result-1280g.json`、`observer2/decoder-log.txt`、`trace-1280g.jsonl`、`native-stop-disasm.txt`、`native-stop-identity.json`。

## 观测器边界及失败保留

仅在私有目录重编一个 board object 后用其余原对象链接；patch 只新增日志和有界 RAM 输入快照，不改 guest 判定、IRQ、队列、尺寸或像素。1280e 曾错误地从 TCG 插件调用虚拟时钟，触发 `Bad icount read`，在播放前退出；该失败保留，不算固件回归。1280f/g 插件只记录明确标注的 host_monotonic_us，模型正常 MMIO 上下文记录 virtual_ns；没有操纵时间。

## 最小后续路径

- 如继续追此 VideoToolbox 样片，只需加 `40719000` 或其调用者 `40719988` 的 w0/w1/LR，以及 `403f7038` 的 event105/payload；先取真正错误分类与发送路径，再决定是否需更下游 probe。不要靠修改 GOP 表、返回成功或补完成 IRQ 获得通过。
- GOP16 与真实 Live 编码器 GOP15 是未排除差异。对用户产品路线更有价值的正例是未来从**真实 1280×852 取景源**经实际 Live 编码器生成片段再验证。当前 capture/live_fit 明确限制 1024，不能为测试偷偷打开更大源，或用上采样替代真实取景输入。
- N2 原型只吞首次 REC edge、后续 held/release 可能透传，是独立的源码生命周期缺陷。此次无 bridge 也失败，故不把修 latch 作为此停播问题的解决方案；留待生产 native bridge 设计时补全并测重复按键。

## Additional single upstream observation (1280h)

`observer-h/summary.json` and its hash manifest preserve one extra exact-F / same-MOV / standard Play+OK run. The original UI callback403f7038 received event04090105 with payload `[80000001,1,0]`, followed by the original periodic poll's mode2 stop. Playback still produced one thumbnail plus16 of90 movie frames; this is not full1280 support.

The sparse40719000/40719988 probes compared full X0 equal to08000000. Neither matched. That filter does not cover a combination bitset or a nonzero upper half and is not evidence that those routines were absent. Static code shows a single08000000/classification5-or6 can construct this event, but the actual generating input/caller remains uncaptured. No further long run was launched and no speculative decode patch was made. A future discriminating observer should record bounded unfiltered entry W0/W1/LR and the native MOVS producer callback40717f30.
