[codex] Turn completed.
发现 **1 项 P1、2 项 P2 缺陷**。未发现新增的错误跳转、现场恢复破坏或必然不开机缺陷；但 D 发码会改变关闭失败判定，而且没有绑定发码调用者，影响本轮诊断可信度。

核验对象为 `linked-lamp-v6`，MAIN SHA256：
`256014ac8efbdd04b19421f1eb02b6a03108263c4666261d7fd7f20fa68478e4`。ELF 加载节与 MAIN 一致，源码清单及 `recorder-build` 镜像一致。以下反例执行实际 AArch64 对象，替代原生调用、时间及异步状态输入；**不是相机观测，也未运行 QEMU**。

1. **P1：持 `close_lock` 发六帧 D，能把成功关闭变成竞争者超时，并提前放行原厂 bring-up。**

   D10、11、13、15、17、18 都在取得 `close_lock` 后、释放前，名义增加至少 **6 秒持锁时间**；竞争关闭者仍只等 4 秒。[关闭副本](/path/to/Nikon/build-tools/livediag/feat-rec/recorder-build/codec/live_w5enc.c:571)

   对象反例让另一关闭者持锁，其他输入保持一致：

   | 持锁者释放时间 | 竞争者结果 |
   |---|---|
   | 0.2 秒 | 正常等待完成，`close_failed=0` |
   | 6.2 秒 | 约 4.001 秒返回，`close_failed=1`、`fin_rc=0x0b000000` |

   **触发条件是并发关闭/抢占调用，不是每次单线程 REC 都必现。** 当前 preempt 接口没有排除第二调用者；它超时后仍返回 notify，原厂 bring-up 可以继续，而首个关闭者尚未完成 FINISH/EXIT。即使首个关闭者后来成功，也不会清掉竞争者写入的 `close_failed`，后续 Live frame 会被该标志拒绝。

   这不仅改变日志或重启时间，确实能制造新的失败状态。不能仅以 README 已声明“扰动超时”消除这个问题。

2. **P2：D 没有调用栈归属检查，非 REC 调用能够冒用正在进行的 REC 会话发码。**

   [bringup 生成逻辑](/path/to/Nikon/build-tools/livediag/feat-rec/src-fix35/recorder_patch.py:78)中，即使 `fr_before()` 因完整 LR 不匹配返回 NULL，armed 分支仍无条件调用 `live_w5_bringup_notify_lamp`。而 [fr_checkpoint](/path/to/Nikon/build-tools/livediag/feat-rec/src-fix35/recorder/recorder.c:171)只检查全局 `claimed`、I 已尝试、未 RETURNED/DONE，不检查当前 SP、LR 或会话指针。

   实际对象反例：

   - 保留一个尚未 R 的 REC 会话。
   - 换用不同栈，保存 LR=`0x40cf9858`，对应基线中确实存在的 MOVS bring-up 调用。
   - 真实 `fr_before` 返回 **0**。
   - 同一外来上下文进入真实 `fr_checkpoint(17)`，仍调用 `emit_frame(4,68)`，即 **D17、NR=0**。

   因此，在调用重叠时，“最后一个 D”可能来自另一条 hand-back 路径。灯所有权只防止同时驱动 GPIO，**不能证明 D 属于目标 REC**。本轮没有证明真机一定发生这种重叠，但对象已证明缺少归属隔离。

3. **P2：D18 推迟 VDI 超时起点，能掩盖原本的错误 9。**

   [D18 位于 EXIT 返回之后、`t=now_us()` 之前](/path/to/Nikon/build-tools/livediag/feat-rec/recorder-build/codec/live_w5enc.c:563)。对象对应 `0x401ecf08` 发 D，`0x401ecf20` 才开始计时。

   用同一异步输入——VDI 在 EXIT 返回后 **1.5 秒**清零——执行实际计时和分支：

   | D18 延时 | 结果 |
   |---|---|
   | 0 | 约 1.001 秒，错误 9 |
   | 名义 1 秒 | 约 1.501 秒，成功 |

   这是可复现的判定变化，不证明真实 VDI 必然这样变化。它意味着 **v6 成功不能反证 v5 没有关闭超时问题**。

其余要求的核验如下。

**插入位置、锁与现场恢复**

| D 阶段 | 发码位置 | 已确认持有的锁 |
|---|---|---|
| 1 | preempt 参数/对象检查之后，alive 检查、发布 `release_req` 之前 | 尚未取得 `close_lock` |
| 2 | busy 检查通过之后，重新检查 alive、竞争 close_lock 之前 | 尚未取得 `close_lock` |
| 10 | 关闭前置检查及可选 SUBMIT 成功之后，STOP 之前 | `close_lock` |
| 11 | STOP 返回且 INFO 基准已采样之后，首次 TRIGGER 之前 | `close_lock` |
| 13 | 首次输出变化已观察到，drain TRIGGER 之前 | `close_lock` |
| 15 | 已观察到 `WSTATE=5`，FINISH 之前 | `close_lock` |
| 17 | FINISH 返回且 worker/实例释放检查通过之后，EXIT 之前 | `close_lock` |
| 18 | EXIT 返回之后，VDI 清零等待之前 | `close_lock` |

D 没有插入原生 Wave5 函数内部。基线反汇编中 STOP、TRIGGER、FINISH 的 service mutex 在返回前释放；D17 在 EXIT 获取相关锁之前，D18 在其返回之后。D 路径没有取得记录器准备锁或执行 FS 操作，未发现新增 FS/Wave5 锁跨 D 持有的证据。

`emit_frame` 在 GPIO 前检查 DAIF.I、任务上下文和调度可用条件；DELAY 返回错误立即停止。GPIO `0x40c15c3c` 是无调用叶函数。因此没有发现新增 ISR 睡眠或关中断发码路径。**这些检查不检测任意隐藏锁，也不保证调度最长延迟；`close_lock` 的实际问题见 P1。**

hook 对象核验通过：

- bring-up 重放 `SUB SP,SP,#0x80`，续行 `0x402f2d5c`。
- S 按原始 W0 重放 CBZ，零/非零分别续行 `0x402f305c/3040`。
- M 重放 `LDP X19,X30,[SP,#0x70]`，续行 `0x402f306c`。
- 784 B 保存帧恢复 x0–x30、Q0–Q31、NZCV、FPCR/FPSR；SP 对齐正确。
- 三处未 armed 门控与旧版字节一致，仍为每处 111 指令／32 B；未 armed 进入原 notify。
- NR 私有 EXIT 重定位、守卫及保存恢复测试通过。

未发现栈损坏证据，但没有据此证明原生任务栈余量。

**各超时与 D 的相对位置**

| 等待／判定 | D 与计时起点的关系 | 可确认的影响 |
|---|---|---|
| busy 4 秒 | D1 后才发布 release_req、起计时；D2 在等待通过后 | 不直接消耗本次 4 秒预算，但推迟冻结新帧和观察 busy |
| close_lock 4 秒 | D2 后起计时；取得锁后才发其余六帧 | 自己的取锁预算不被 D 消耗；竞争者会受 P1 影响 |
| 首次 TRIGGER 原生事件等待，参数 1000 | D11 在调用之前 | D 不在该等待内部；推迟命令提交 |
| 输出变化 500 ms | 首次 TRIGGER 返回后起计时 | D11 不直接消耗预算 |
| drain TRIGGER 原生事件等待，参数 1000 | D13 在调用之前 | 同上 |
| 等待 worker **状态 5**，1 秒 | drain TRIGGER 返回后起计时 | D13 不直接消耗预算；“worker 5”不是五秒 |
| FINISH：发 `0x400`、等 `0x800`，参数 100 | D15 在整个调用之前 | 不直接消耗该等待预算；FINISH 内没有 D |
| EXIT 内 VDI 退出事件：发 4、等 8，参数 1000 | D17 在调用前，D18 在返回后 | 没有内部阶段码，无法仅凭 D17 区分 EXIT 内部子步骤 |
| EXIT 后 VDI 清零 1 秒 | D18 后才起计时 | 已复现 P2 掩盖超时 |
| preempt 后 3 秒 reopen 冷却 | D 流程结束后写 `preempt_us` | 冷却窗口整体后移 |

原生事件等待参数由反汇编确认；这里没有未经校准地把其单位写成毫秒。也没有证据把 v5 的约 28 秒重启直接归因于某个 `20000` 等待或看门狗。

**记录器 I/O、编码及互斥**

REC 的记录器文件 I/O 已删除。最终 `fr_before` 只有 `fr_context` 一个调用，无间接调用；I 是之后的首个发码调用。链接对象中，`persist` 的调用只来自准备路径，旧 `live_encoder_diag_tick` 虽仍链接但未找到调用点。

准备事务保留，worker 的 photo 条件与 `fr_prepare` 内照片模式复查均存在。[准备门控](/path/to/Nikon/build-tools/livediag/feat-rec/src-fix35/recorder/recorder.c:82)只能保证**开始前检查为照片模式**；模式随后改变时，已经开始的事务仍可能继续。这不是本版新增缺陷，也不能扩称为“REC 后全系统无文件活动”。

D 编解码一致：`100 | 阶段[5] | NR[2] | CRC4[4]`，半位 25 ms，名义一帧 1 秒；八个阶段、四种掩码、逐 bit 错误及 120 fps 合成采样测试通过。I/R/S/M 格式保持。

D 与 R 使用同一个原子灯所有权；R 先发布 RETURNED，D 检查取消并释放所有权，R 竞争失败则停止发码。没有发现两者同时拥有记录器 GPIO 的路径。原厂灯写入不受该所有权控制；缺码仍不能当成未到达。

**下一次 A/B 观测的解释边界**

当前 v6 应先解决上述缺陷；下表是解释规则，不是上机批准。

| 有效观测 | 能证明什么 |
|---|---|
| 无 I/D/R/S/M | 没有有效灯证据；不能证明入口未执行 |
| 只有 I | 精确 REC LR 已匹配、会话已领取并到达入口发码点；不证明准备成功或 close 开始 |
| 有 D，无 R/S/M | 某条获准报告的关闭路径到达该检查点；当前版须保留 P2 外来调用解释 |
| 有 R | 目标 REC 的 notify 已返回；不证明 close 成功 |
| 有 S，无论是否解出 R | service 已返回；此前 notify 已返回，S 是原始返回值 |
| 有 M，无论是否解出 R/S | 已到映射汇合点；不证明 bring-up 已 RET 或录像成功 |
| R/S/M 都有 | 可联合检查返回值与映射，仍需录像结果证据 |

“最后一个 D”只能给出**已到达的下界**。在排除外来调用且后续无码确实不是漏解的条件下，可用上面的阶段表缩小待查区间；不能直接写成“卡在下一函数”。例如：

- D2 后无后续码：包含 close_lock、前置检查、可选 SUBMIT，也可能是后续漏码。
- D15 后无 D17：包含 FINISH、返回后的 RAM 记录及状态检查。
- D17 后无 D18：包含 EXIT，但尚不能证明 EXIT 已实际进入。
- D18：EXIT 已返回；不证明 VDI 已清零或 close 成功。
- worker 自己卡在原关闭路径时，这个同步副本不会报告 worker 的内部进度。

A 应观察到 NR 掩码 0。B 中任一有效 D 或 R 的掩码 3，证明入口清零后的证据窗口内两种跳过都被记录；1/2 仅证明对应位，不保证另一次没跳过，沿用 REVIEW8 的交错限制。B 只有 I、D17/0 或缺 R，都不能确认跳过已经执行。

A 失败、B 有掩码 3 且录像成功，支持跳过操作影响结果；两组都失败说明跳过不足以避免故障。由于本版自身改变等待和竞争，两者都不能单独锁定 NR 为唯一根因。`S=0、M=0/1` 也不是最终录像成功证明。

本轮只读复跑通过：576 条保存恢复路径、1680 条门控路径、288 条 NR 守卫及 6 条作用域路径、8 项解码测试、关闭副本结构检查。会写报告的测试仅在内存中移除报告输出；未修改文件。已知 `verify_link` CFG 不完整不列为新增缺陷，也未将其 FAIL 改称 PASS。

**判决：不接受当前 v6 进入完整链接和 QEMU 验证阶段。** 阻断项是新增持锁延时可制造关闭失败，以及 D 缺少调用者归属；VDI 超时被掩盖也须处理。全程未使用 USB、刷写、操作相机或 QEMU。


[exited with code 0]
