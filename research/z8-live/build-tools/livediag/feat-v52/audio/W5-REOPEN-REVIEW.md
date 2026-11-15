# v52 Eclose 重新打开失败：独立审查

后续 Eclose passive diagnostic 已确定：第二次 `SET_PARAMS` service 返回 0；`HEADERS` service 返回 -1，且没有进入 VPU header command 的返回点。原厂 `HEADERS` worker 的 state!=2 检查提前拒绝了请求，之后才观察到 state 2。下文保留最初分支分析；尚不能据此宣称 G 整机恢复已通过。

F 曾把 `0xa02d32788==0` 增加为等待条件，但这被实际 E 成功编码时该槽为 -1 的反例推翻。独立 ARM 失败路径审查支持 G 删除该额外条件，同时保留当前请求前 state 0、原 SET_PARAMS 返回成功、实际 state 2、完整 owner/instance 绑定和截止时间；见 `W5-READY-G-REVIEW.md`。

## 已观察事实

实跑目录 `/path/to/user/.local/share/nikon-z8-qemu/live-audio/regress/runs/v52e-hevc-close`，执行 MAIN `/path/to/user/fxfix/v52-root/v52eclose/q/main.main` SHA-256 `3b976636e4b67c86a9e93b0d82c598d23a7ea3e73c4733b3a7b82b27f252e7cb`。

关闭后曾观察到 VDI 0、client map ff、worker 0、Live hw 0；随后新分配状态里 VDI `0x16010000`、client 4→slot 0、slot used 1、实例 `0xa02d41848` active 1 / codec 1，但 worker 一直为 2，Live magic 0、busy 0、HEVC magic 0、frames 0。IRQ 继续增长，编码总数 554 不增长。

`open_stream()` 失败后已经返回，后续 worker 还在运行。`capture.reason=1` 是上层通用失败，不是 pool allocation 失败的充分证据。

## 原厂指令结果

- `0x40c3d898..d8b8`：初始化、帧缓冲分配/注册成功后写 `0xa02d32788[stream]=0`、worker state 2，再发 event `0x20`。
- `SET_PARAMS 0x40c38730`：发 event `0x10`，等 `0x20`（1000 ms）；超时写 `0xa02d32768[stream]=-3`，返回也读 **2768**。它与 worker 写的 **2788** 不同。成功 wait 不会清除 2768 的旧值。尚未证明 Eclose 的 2768 实际为何值，不能仅据此宣布根因。
- `HEADERS 0x40c38af8`：发 `0x40`、等 `0x80`，返回 `0xa02d32770[stream]`；超时写 -3。
- worker `0x40c3b668` 要求 state 2；否则写 2770=-1 并发 `0x80`。状态正确时调用 `0x40c35048(instance,0x33,params)`。命令返回 0 立即写 state 3；非 0（除 0x19 重试）写 2770=-1，保留 state 2。
- HEADER 完成中断 `0x40c3c750` 以后处理结果。成功提交已经推进 state 3，因此“唯一是 Live 的 hdr 高 32 位地址检查失败”不符合最终 state 2，除非另有未观察到的状态变更。
- 命令 0x33 最终 `0x40c5a268` 写 W5 `ENC_PIC 0x100`，header flags 0x3c 写寄存器 0x164；它等 busy 结束再读 success 0x108 / fail reason 0x10c。超时返回 0x10；reason 1 返回 0x19，reason 0x40 返回 0xf，reason 0x20000 返回 0x10，其余失败返回 1。

独立执行原厂 ARM 分支的 `test_native_w5_reopen_states.py` 通过。结果 `native-w5-reopen-states.json` 保留 5 种 HEADER state/返回值和 4 种 SET_PARAMS wait/返回槽组合。相关 5 段原厂机器码与 Eclose MAIN 的 hash 已逐段匹配。测试仅 stub 掉 VPU command 和 RTOS event wait/post，证明分支行为，不证明哪条分支发生在整机实跑。

## 最小判别观测

建议只读 observer：

| PC | 读值 | 意义 |
| --- | --- | --- |
| `0x40c38a74` | w8 | SET_PARAMS service 最终返回值 |
| `0x40c33df0` | w0 | HEADERS service 返回值 |
| `0x40c3b6d4` | w0 | HEADER VPU command 返回值 |

同时记录 `0xa02d32748..0xa02d327a0` 和事件处理前的 worker state。没有第二次硬件 HEADER 命令，仍无法区分 SET_PARAMS 未通过和 HEADERS 在 worker state 1 被拒绝；后者可能随后才由异步初始化推进到 state 2。

## 确认的错误路径问题

最终 source `integration-eclose/work/src-fix37/codec/live_w5enc.c` 在 fresh `open_stream` 失败时直接返回 rc，保留 `e->hw`，却不设置 W5 magic。已有状态重开失败则主动将 magic 清 0。若 BRINGUP/instance creation 已成功，这会把正在被 native 引用的 slot/arena 留在“无效 Live 状态”里；`preempt`、`dispose` 等正常清理路径又拒绝该 magic。这是永久失去恢复/清理能力的真实错误路径。

修复需要显式保留已取得的资源/初始化阶段并按该阶段进行安全终止。不得无条件 free、伪造成功、清全局占用或直接重复 BRINGUP。先观测确切失败阶段，再设计对应终止；本次没有改共享源。
