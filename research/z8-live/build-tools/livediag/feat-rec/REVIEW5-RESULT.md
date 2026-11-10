[codex] Turn completed.
**判决：不接受进入完整链接和 QEMU。** 主要阻断项是：准备失败仍能写 REC 扇区，以及文件证据仍无法完成要求的五分类。

本轮未修改文件，未使用 USB、刷写、操作相机或运行 QEMU。以下对象地址均为 `.text` 内偏移；固件地址来自已核实 SHA-256 为 `c7c6dbee…fe4d4` 的 `bg34.main`。

1. **[P1｜反汇编与反例证明] 五分类目标没有实现，记录器自身挂起仍会冒充被测故障。**

   `hooks.S.o` 顺序确为：

   ```text
   +0x6ac  BL fr_before
   +0x6b4  BL live_w5_bringup_notify
   +0x6c0  BL fr_enabled
   +0x6d0  BL fr_probe
   ```

   但事务阶段只更新 RAM；扇区在本次事务开始前构造，没有当前 WRITE/CLOSE 的返回证明。见 [recorder.c:77](/path/to/Nikon/build-tools/livediag/feat-rec/src-fix35/recorder/recorder.c:77)。

   两个确定反例：

   - **未到 hook**与**到 hook 后卡在入口 OPEN**，可以留下完全相同的文件：只有事件 0。
   - **入口写完后卡在 CLOSE**、**入口事务完成后卡在 hand-back**、**hand-back 返回后卡在事件 1 的 OPEN**，可以留下完全相同的文件：事件 0、15 有效，其余全零。

   因此新增记录增加了正向证据，但未满足 FIX5 的核心定位目标。

2. **[P2｜对象反汇编证明] `file_owned` 不是“准备成功”，准备失败后仍可写入口、拒绝扇区。**

   [recorder.c:99](/path/to/Nikon/build-tools/livediag/feat-rec/src-fix35/recorder/recorder.c:99) 在排他 OPEN 成功后立即置 `file_owned=1`，尚未执行 WRITE/CLOSE。对象对应：

   ```text
   +0x804  STR w8,[x19,#0x3c]   ; file_owned=1
   +0x82c  BLR …               ; WRITE
   +0x900  BLR …               ; CLOSE
   ```

   后续非创建事务在 `+0x720…724` 只检查 `file_owned`；`fr_before` 不以 `ready` 阻止写入。

   **具体反例：**准备 WRITE 写足 3072 字节，CLOSE 返回错误。此时 `ready=0、file_owned=1、文件长度=3072`。随后精确 LR 进入，入口事务通过长度检查，写偏移 512；接着还能写偏移 1536 的拒绝记录。全过程没有准备成功，违反“准备成功前不会写 REC 扇区”。

   现有“排他创建失败不覆盖旧文件”测试不能覆盖这个反例。

3. **[P2｜对象解释器反例证明] hand-back 调用前没有完整恢复寄存器、NZCV 和原调用栈位置。**

   [recorder_patch.py 的 `bringup()`](/path/to/Nikon/build-tools/livediag/feat-rec/src-fix35/recorder_patch.py:72) 在 `fr_before` 返回后只执行：

   ```text
   +0x6b0  LDR x0,[sp,#0x210]
   +0x6b4  BL live_w5_bringup_notify
   ```

   中间没有恢复 `x1–x7`、NZCV 或撤销完整保存帧。限定指令解释器中的调用破坏反例显示：hand-back 入口 `x0` 正确，`x1–x3/NZCV` 已改变，SP 为入口 SP−784；旧基线调用时为 SP−256。

   **影响边界：**当前 notify 只使用 `w0` 参数，所以没有证明实际 client 被破坏；回到 stock 前，完整状态恢复测试通过。但不能宣称“写卡后、调用 hand-back 前完全还原”。

4. **[P2｜基线反汇编证明] “hand-back 函数体逐字不变”只对现有 v35 源码成立，对指定 `bg34.main` 不成立。**

   构建对象 `live_encoder_preempt` 在 `+0x1360、+0x1440、+0x1468` 调用 `w5_diag`，分别记录阶段 1、2、30；基线 `0x401e86d4` 开始的对应函数没有这三处调用。对应 [live_w5enc.c:379](/path/to/Nikon/build-tools/livediag/feat-rec/recorder-build/codec/live_w5enc.c:379)。

   这是继承自 `src-fix35` 的差异，并非此次入口记录新增；但以 **bg34 为基线**，不能通过“源码只追加 snapshot”的检查证明 hand-back 未变。

其余重点的核验结果：

- **入口顺序、返回错误路径：**事件 15 确在 hand-back 前；只要记录函数返回，无论成功或错误，汇编均继续调用 hand-back，没有依据写入结果跳过 hand-back 的分支。同步调用不返回则无法继续。
- **未 armed 成本：**限定对象解释器通过 3360 条测试路径；六入口有效链 `arm=0` 都是 **111 指令、32 字节 scratch**。这是既有 recorder 门控成本；裸 bg34 本来没有这段门控。
- **布局与版本：**六槽偏移为 `0/512/1024/1536/2048/2560`，末端恰为 **3072**。对象预分配、EOF 检查、decoder、arm layout 一致；v3 魔数/版本检查拒绝旧版记录。源目录与构建目录对应文件一致。
- **拒绝记录：**入口检查完整 64 位 LR=`0x40d07ba8`；后置路径也经过该精确匹配。`reject_claimed` 在 prepare 中不重置，保证每 boot 最多一次拒绝**尝试**；锁失败也会消耗机会，故不保证留下记录。未发现错误 LR 导致拒绝写卡的路径。精确 LR 证明调用点身份，不能单独替代对所有开机、拍照、回放执行路径的证明。

文件判定如下。`P/E/A/R/S/C` 分别指事件 `0/15/1/16/2/13`；“有”指版本、槽位、校验均有效。

| 实际情况 | 可能文件形态 | 能证明什么／残余 |
|---|---|---|
| 未到 hook | 仅 P，其余零 | 不能排除入口 I/O 阻塞、runtime/arm 无效 |
| 门控拒绝 | 通常 P＋E＋R，无 A | R 给出原因；仅 epoch 位是标记，不代表拒绝。R 写失败时仍可能没有证据 |
| 卡在 hand-back | P＋E，可附调用前 R；无 A | 与入口 CLOSE 阻塞、返回后记录阻塞重叠 |
| hand-back 返回后出事 | 有 A，或 R 带 `FR_AFTER`；更晚可有 S/C | 这些是返回的正证据；若在它们落卡前出事，也可能只有 P＋E |
| 记录器 I/O 卡住 | 仅 P、P＋E、有 A，或当前槽零／损坏／有效 | 最后一条有效记录不证明其 CLOSE 返回；不能唯一识别 I/O 挂点 |

另外，**P 本身也不是准备成功证明**：它在准备 WRITE/CLOSE 前构造。

关于死锁风险，**共享 FS 锁和新增阻塞点已证明，循环死锁尚未证明**：

- 基线 OPEN/WRITE/SEEK/CLOSE 分别在 `402dd524/402dd7d0/402dd918/402dd578` 对同一对象 **`0xa02e81dd8`** 调用 `40d2bfb4`。
- 争用路径进入 `401815cc`；`401816bc…16ec` 将当前任务挂入等待并调度，没有该路径的超时参数。
- Live 的 native file wrapper 使用同一组入口。因此 recorder 的单次 `take()` 不能避免底层 FS 阻塞。
- 入口事务发生在 `live_encoder_preempt` 设置 `release_req` 之前；FS 等待会推迟 hand-back，其内部四秒等待限制也尚未开始。

所以，**新增同步写卡可以成为 hand-back 前的新挂起点**。但尚未找到“FS 持有者必须等待当前 REC hand-back 才能释放”的闭环证据，不能将锁竞争直接写成“已证明死锁”。

本轮重跑通过 decoder/layout、3360 条门控路径、32 条保存恢复路径及 1911 个既有 forwarding 负例。会生成文件的 host I/O／完整合成链接测试未重跑；旧链接回归不算 v3 链接验收。`verify_link` 的已知 CFG 不完整未计作缺陷。

