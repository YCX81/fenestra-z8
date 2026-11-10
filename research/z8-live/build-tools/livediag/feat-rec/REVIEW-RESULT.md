[codex] Turn completed.
发现两个确定的运行逻辑缺陷，以及一处检查结果夸大。**现有材料不足以批准刷写。** 本次只读核对了源码、对象文件和 `bg34.main`，未修改文件、连接 USB 或刷写；MAIN 的 SHA-256 与清单一致。

1. **高：所谓“无新增 boot I/O”并不成立，正常开机进入照片模式就会自动创建文件。**

   位置：[生成后的 runtime.c:230](/path/to/Nikon/build-tools/livediag/feat-rec/recorder-build/runtime.c:230)、[recorder.c:71](/path/to/Nikon/build-tools/livediag/feat-rec/src-fix35/recorder/recorder.c:71)。

   【源码证明】门控只有 PHOTO、未 shutdown、无 writer/job、`LA_READY`。Live 在照片模式积累足够帧后就能进入 `LA_READY`，不需要按 REC，也没有独立的诊断启用动作。随后立即执行同步 OPEN、16384 字节 WRITE、CLOSE。

   **失败场景：**开机恢复 Live、进入照片待机后，文件系统锁或底层卡操作不返回，Live worker 就停在预创建事务内；这发生在任何 REC 测试之前。`tried=1` 只防止再次调用，不能限制正在进行的调用。即使已有 `W5FR.BIN`，也必须先进入 OPEN、取得文件锁，才有机会返回“已存在”。

   因而 [README:110](/path/to/Nikon/build-tools/livediag/feat-rec/src-fix35/recorder/README.md:110) 的“无新增boot文件操作”是错误的。**没有证据证明它一定复现 v26 的重启循环，也没有证据证明 v26 的根因与此相同；确定的问题是启动隔离条件不存在。**

   最小改动：增加明确的、启动后主动启用的准备门控；不能用 PHOTO/idle 代替。若采用离线预置文件，必须改为验证并使用专用准备文件，当前排他创建逻辑会直接拒绝它。

2. **高：正常锁争用也会导致 `active` 永不清除，随后在照片／其他调用中继续记录旧 REC 会话。**

   位置：[recorder.c:93](/path/to/Nikon/build-tools/livediag/feat-rec/src-fix35/recorder/recorder.c:93)，尤其第 96、100–103 行。

   【源码证明】争用分支只设置 `missed[event]`，立即返回；`seen` 更新与结束会话判断均被跳过。

   **具体失败时序：**

   - adapter 正在某个探针中持有 recorder 锁并写卡。
   - MOVRS 到达事件 5，取锁失败，设置 `missed[5]`，继续进入 stock 等待。
   - adapter 后续到达事件 12，成功记录并发送完成事件。
   - REC 可以正常启动，但 `seen` 永远缺少 bit 5，`active` 保持 1。

   反过来，事件 12 撞上事件 5 的写入，也有相同问题。它**不要求 REC 异常或挂起**。

   此后照片 Live 的 `open_stream()` 会调用同一个 bring-up/service 路径，触发事件 2。`fr_probe()` 不核对当前 client、任务或 REC 代次，便使用上次保存的 REC 请求字段写入。这既污染判定表，也把文件操作带入本应未激活的照片路径。

   最小改动：将“事件已到达／会话结束”与“成功取得写锁”分离，以并发安全的状态更新处理被丢弃的事件；再对共享探针核对本次 REC 身份。不能仅在争用分支补一个普通 `seen |= …`，那会引入另一处数据竞争。

3. **中：`results.json` 的“relocations PASS”超出了检查实际覆盖范围。**

   位置：[results.json:6](/path/to/Nikon/build-tools/livediag/feat-rec/recorder-build/recorder-check/results.json:6)、[verify_static.py:16](/path/to/Nikon/build-tools/livediag/feat-rec/src-fix35/recorder/verify_static.py:16)。

   【源码证明】该静态检查验证的是 MAIN 原指令、原目标、hand-back 源码一致性和 recorder 原子指令；**没有检验最终链接后的 trampoline 重定位、patch B 目标或全部旧 hook 重定向**。执行测试又未完成，完整链接也不存在。

   **失败场景：**完整 relive 接入时漏对象、漏旧 hook 重定向、使用错误区域上限，现有这个 PASS 仍不能发现。不是已经证明存在某个链接错误，而是该 PASS 不能充当发布门槛。

   最小改动：收窄结果描述，并让最终 ELF／MAIN 的区域、符号、分支和修改白名单检查成为强制失败条件。

逐探针独立核对结果如下。表中原字节均直接匹配本次读取的 MAIN；未发现 displaced instruction 本身的重定位错误。

|事件|地址／原字节|恢复行为|执行上下文|
|---|---|---|---|
|1|`402f2d58 / 0fc9fb17`|替换既有 hook；恢复 `sub sp,sp,#0x80`，回 `402f2d5c`|共享 bring-up；仅精确 REC LR 激活写入|
|2|`402f303c / 00010034`|零→`402f305c`，非零→`402f3040`|共享 service 返回，包括照片 Live|
|3|`40d07ba8 / e00315aa`|恢复 `mov x0,x21`|MOVRS_F|
|4|`402f5870 / f37bbfa9`|恢复压栈 `stp x19,x30,[sp,#-16]!`|adapter 启动调用者；本链为 MOVRS_F|
|5|`402f58c8 / 0fd92894`|LR=`402f58cc`，跳 `40d2bd04`|MOVRS_F，等待 adapter 前|
|6|`402f4624 / c0000034`|零→`402f463c`，非零→`402f4628`|adapter 任务|
|7|`402f46d8 / c0000034`|零→`402f46f0`，非零→`402f46dc`|adapter 任务|
|8|`402f4810 / c0000034`|零→`402f4828`，非零→`402f4814`|adapter 任务|
|9|`402f486c / 00010034`|零→`402f488c`，非零→`402f4870`|adapter 任务|
|10|`402f48b0 / c0000034`|零→`402f48c8`，非零→`402f48b4`|adapter 任务|
|11|`402f4aa8 / c0000034`|零→`402f4ac0`，非零→`402f4aac`|adapter 任务|
|12|`402f4c18 / 1ddc2894`|LR=`402f4c1c`，跳 `40d2bc8c`|adapter 任务，发送完成事件前|

【源码及对象反汇编证明】[hooks.S:4](/path/to/Nikon/build-tools/livediag/feat-rec/recorder-build/hooks.S:4) 的保存／恢复覆盖 x0–x30、NZCV、q0–q31、FPCR/FPSR；保存区为 `256+528=784` 字节，保持 16 字节栈对齐。`regs=sp+528` 正确指向整数寄存器保存区。CBZ 展开不改变 flags；两个 BL 的替代正确恢复原 LR；没有借 x16/x17 做绝对跳转，也没有搬错 ADR/ADRP/LDR literal。最终链接仍须验证实际分支和 literal relocation。

但这些保存操作**发生在 C 门控之前**：照片 Live 首次 bring-up 也会执行新的 SIMD 保存和增加的栈占用。当前没有证明因此溢栈或触发 FP 异常，不能把它报成确定故障。已追到的 REC 和 adapter 路径属于任务上下文，未发现这里是 IRQ 回调；这也不等于已经证明所有上层锁条件允许文件 I/O。

关于同步写入，结论比“锁可能有问题”更具体：

- 【反汇编证明】OPEN、WRITE、SEEK、CLOSE 分别在 `402dd524 / 402dd7d0 / 402dd918 / 402dd578` 获取 **`0xa02e81dd8`**。recorder 持自己的锁跨越整个事务，因此顺序是 **recorder 锁→FS 锁→底层文件操作**。另一任务只尝试一次 recorder 锁，不等待；未发现由 recorder 两任务互等直接构成的锁环。
- **尚未证明 stock 持 FS 锁等待 MOVRS／adapter 的反向依赖。不能声称已找到确定死锁。** 但同步 native 调用没有本地退出上界，这是实际代码性质。
- **它确实能制造与被测症状相同的停顿：**事件 1 写入不返回，stock bring-up 根本尚未开始；事件 12 写入不返回，完成事件永远不会发送，MOVRS 会继续等待。hand-back 源码虽然未改，继续执行 stock 的时间和存活性已被文件系统绑定。
- **离线文件不能可靠区分“下一 stock 步骤挂了”和“下一条日志的 OPEN/SEEK/WRITE/CLOSE 挂了”。** 灯在事务期间亮只能提供有条件的现场辅助；stock 也能操作同一灯，而且文件里没有本次事务的逐阶段完成证据。[persist:55](/path/to/Nikon/build-tools/livediag/feat-rec/src-fix35/recorder/recorder.c:55) 把结果留在 RAM，下一条才携带上一笔结果。

栈与 DMA 方面，没有找到可以证实的对齐错误，但也没有安全闭环。普通探针增加 784 字节保存区，`.su` 另给出 `fr_probe=48`、`persist=112`；WRITE wrapper 自身再用 `0x60`，下一层 `408b6db8` 用 `0x110`，尚未包含更深文件系统调用。不能拿单个 `.su` 当任务总栈上界。缓冲区位于 runtime 堆对象、对齐 16 字节，写前执行 cache clean/barrier，并转换缓存别名；这与现有 native file 包装一致，**没有证据要求这里必须 512 字节地址对齐，也没有足够证据认证所有卡驱动 DMA 路径接受该分配。**

无卡、卡满和创建失败的行为是：

- native OPEN 正常返回失败：熄灯，`ready` 保持 0，`tried` 已为 1；之后插卡也不再准备。
- 创建后短写／写失败：尝试 CLOSE，停用；可能留下零长度或部分文件，下次启动又因排他创建被拒绝。
- CLOSE 返回失败：停用。
- 任一 native 调用不返回：上述失败处理均执行不到。
- 已有文件不会按当前 flags 被截短；SEEK 的布尔返回值、64 位位置参数及实际偏移检查与反汇编吻合。

payload 范围应为 **`[0x401e2000,0x401fff80)`，122752 字节**。新增 16 KiB 缓冲是动态 runtime 内存，不应直接算作 RX payload 膨胀。四个已编译对象未发现新增 `.data/.bss`；但缺完整 ELF，无法给出最终尺寸、全部符号解析和旧 hook 重定向的通过结论。旧 `build.py` 的 `0x40200000` 上限确实不能使用。

decoder 的字段偏移、64 位组合和 `fin_rc >>24` 对照生产端未发现错误；本次内存测试通过有效、校验损坏和截断情况，现有 host stub 二进制也正常退出。判定表对“最后记录≠最后完成调用”“事件12≠事件已发送”“rc4≠必然 arena 不匹配”的限制基本正确。**但第 2 项会破坏其“记录属于这次 REC”的前提；修复前不能按连续单次 REC 时间线解读文件。**

**最终判决：不能批准当前版本作为安全诊断构建并刷写。** 最少需要：明确的启动后准备门控；修复争用时的会话结束与身份校验；完成全量链接和最终镜像检查。同步写入还必须补足任务栈／锁依赖／DMA 的验收依据，以及独立于该文件事务的挂起识别证据；若仍只依赖同一路径同步写卡和一个共享灯，就不能声称它不会制造、或能够可靠区分正在测量的挂起。


[exited with code 0]
