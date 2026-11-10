[codex] Turn completed.
**仍有三项缺陷，当前不接受作为 armed-only 诊断版进入刷写。** 本次全程只读，无 USB、无刷写。`bg34.main` SHA、manifest 中的源码/生成文件哈希均匹配，两份 recorder 源码一致。

1. **高：合并事件会留下旧 `pending`，后一次 REC 的完成可被记入前一次会话。**

   位置：[recorder.c:128](/path/to/Nikon/build-tools/livediag/feat-rec/src-fix35/recorder/recorder.c:128)、[recorder.c:141](/path/to/Nikon/build-tools/livediag/feat-rec/src-fix35/recorder/recorder.c:141)。

   【源码及反汇编证明】事件14只接受 `r[0]==1`。但 stock 回调处理的是事件位图：`0x402f4580` 检查 bit0，`0x402f4c1c` 检查 bit1；合并输入 `3` 会执行启动及停止分支。

   失败时序：

   - 第一次 REC 发布 `pending=gen`；adapter 收到合并事件 `3`。
   - 事件14被拒绝，**pending 没有被消费或作废**；事件13因 `adapter_gen` 未设置也被拒绝。
   - MOVRS 等待返回，记录事件5，但缺少13，故没有 DONE。
   - 后一次 REC 的事件1被 `claimed` 拒绝；然而其纯事件 `1` 回调仍能消费旧 pending，绑定新回调栈。
   - 后一次完成通过事件13校验，用第一次的 session、请求参数和文件写入完成记录。

   因此 README 所说的“混合事件保守漏记”和“一个 boot 不会混入后续 REC”并不成立。`claimed` 只禁止重新初始化身份，没有禁止旧身份被后续回调使用。

   **最小修复：**对携带启动位但不接受关联的回调，也必须原子消费／作废 pending，并终止该诊断会话；不能在外层门控直接返回而留下可复用 token。增加“合并回调→下一次 REC”的回归用例。

2. **高：最终检查器会放过旧 trampoline 的错误返回地址。**

   位置：[verify_link.py:47](/path/to/Nikon/build-tools/livediag/feat-rec/src-fix35/recorder/verify_link.py:47)、[verify_link.py:69](/path/to/Nikon/build-tools/livediag/feat-rec/src-fix35/recorder/verify_link.py:69)。

   独立校验的 continuation 只有 bring-up 和新增探针。旧的 `live_irq_resume`、`live_start_resume`、`live_closed_resume` 等，重定位时直接相信待验 ELF 的符号值；因此只能证明“对象按这个值链接了”，不能证明这个值正确。

   【本次内存复现】使用现有 synthetic ELF 重建测试 MAIN，检查器返回 **PASS、20 个 patch sites**，其中：

   |符号|被接受的错误地址|应有地址|
   |---|---|---|
   |`live_irq_resume`|`0x401e3268`|`0x40e9083c`|
   |`live_start_resume`|`0x401e3280`|`0x40c26808`|
   |`live_closed_resume`|`0x401e3250`|`0x408b7c20`|

   这些错误落点是 fixture 的 `ret` stub，真实运行不会恢复正确的 stock 控制流。**这证明检查器存在漏检，不代表当前尚未完成的真实链接已经有该错误。**

   **最小修复：**独立固定全部旧 continuation，并核验旧转发目标；加入“错误 ELF 符号＋对应错误分支仍彼此一致”的负例，要求失败。

3. **中：未 armed 的 trampoline 仍不是要求的轻量早退。**

   位置：[recorder_patch.py:47](/path/to/Nikon/build-tools/livediag/feat-rec/src-fix35/recorder_patch.py:47)、[hooks.S:137](/path/to/Nikon/build-tools/livediag/feat-rec/recorder-build/hooks.S:137)、[runtime.c:50](/path/to/Nikon/build-tools/livediag/feat-rec/recorder-build/runtime.c:50)。

   【对象反汇编证明】arm 为零时，每次命中仍先保存全部 x0–x30、NZCV，再经过：

   `fr_enabled → fr_context → live_ui_engine_get → ui → core_root`

   新门控路径峰值栈占用是 **336 字节**：汇编256，加上述 C 调用链80。仅汇编保存／恢复就有32条成对访存、512字节栈读写；之后才查到 arm 为零。共享 bring-up 还在原 hand-back 保存／恢复之后，再完整做一次。

   **失败场景：**普通照片 Live、播放或启动过程命中这些共享入口，即使从未 arm，仍支付上述成本。worker 的零 arm 分支足够轻，但 trampoline 没达到 FIX.md 的要求。没有证据证明这已经导致栈溢出或重启，不能将其升级为确定硬件故障。

   **最小修复：**把零 arm 检查放到全寄存器保存和 C 调用之前，使用少量明确保存的临时寄存器完成只读查找；完整保存仅用于已启用路径，并验证拒绝路径的寄存器、NZCV和栈恢复。

上次发现的修复状态：

- **未 arm 自动建文件：已修复。** worker 和准备函数均有门控；现有文件排他拒绝、失败后使用更大 token 重试的逻辑未发现新错误。
- **写锁争用阻止结束：该具体问题已修复。** 到达位和 DONE 使用原子更新，不依赖写锁或 ready；但会话隔离仍有第1项漏洞。
- **“relocations PASS”夸大：结果措辞已修正；最终验证仍有第2项漏洞。**
- **arm 地址推导正确：**与 UI 注册表链及 AArch64 layout 对象一致，当前为 `runtime + 0x5a90`，runtime 大小25424；不是固定绝对地址。
- 五个新增探针的原字节、恢复分支及栈/LR关系与基线一致。事务 phase 和文档已明确离线文件／共享灯仍不能可靠区分最后一次 recorder I/O 与 stock 停顿；本次未把这种已声明的诊断限制另报成新缺陷。

只读运行的 decoder、门控结构检查及现有 host 测试二进制通过；真实完整链接和硬件执行仍未验证。

**最终判决：不接受当前版本按“完整链接后即可刷写的 armed-only 诊断”推进。最少先修复旧 pending 跨 REC 复用、旧 hook 返回目标漏检，以及未 armed 的重型门控，再执行全量链接和最终镜像检查。**

[exited with code 0]
