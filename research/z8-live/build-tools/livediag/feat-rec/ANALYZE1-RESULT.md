[codex] Turn completed.
**【已证明】当前证据只能说明“事件 1/2/13 没有留下有效记录”，不能定位死机发生在 hand-back 内还是之后。发现两个明确的诊断盲点：client 门控可能漏掉真实 REC 调用；卡代次失效时 `ready` 仍可显示为 1。尚未证明普通 Live 拍摄必然触发它们。**

本轮只读检查，无文件修改、USB、相机操作、刷写或 QEMU。

**【已证明｜对象、ELF 与文件】**

- 本地 MAIN 的 SHA-256 确为 `ce1879c8…`，ELF 全部 ALLOC 段与 MAIN 对应字节一致。
- 实际对象没有独立 `fr_entry_1` 符号；事件 1 使用共享入口 `live_w5_bringup_entry=0x401e5230`。
- 已核对调用顺序：`402f2d58 → 401e5230 → hand-back → fr_enabled → fr_probe → record → persist → stock continuation`。armed 路径的 hand-back 调用在 `401e5498`，事件探针在 `401e54b4`。
- 实际 BIN：扇区 0 校验有效、事件 0、epoch=1；其余三个扇区**全零**。文件预先分配为 2048 字节，后续只是原位覆盖，因此长度不变没有定位意义。事件 0 不采集 Wave5 状态，其零字段不能解释为资源已经释放。

下表穷尽已识别的正常分支及停顿区间；任意内存破坏、异常或任务停止单独归入最后一项。依据为真实反汇编及对应的 [recorder.c](/path/to/Nikon/build-tools/livediag/feat-rec/recorder-owner/recorder/recorder.c:85)。

| 事件 1 未落卡的路径 | 现有证据能否排除 |
|---|---|
| REC 在 `402f2d58` 之前失败或阻塞 | **不能。** v34 灯只支持那次执行曾到 hook，不能替代本次入口证据。 |
| 汇编门控拒绝：root/core/registry/UI/runtime 的范围、对齐、身份、尺寸或 arm 不符 | **不能。** 准备成功仅证明此前状态可用；拒绝后仍执行旧 hand-back，不记录拒绝原因。 |
| hand-back 自身不返回 | **不能。** busy/close-lock 等待有软件超时，但 STOP/TRIGGER/FINISH/EXIT 等 native 调用没有由外围超时保证返回。v34 灯熄灭削弱此假设，但不是本次返回证明。 |
| `fr_context` 拒绝：shutdown、epoch 不同、arm=0、arm≠generation 或 runtime 无效 | **不能。** 这些检查发生在 hand-back 后，且没有落卡拒绝记录。 |
| LR/client/模式门控拒绝 | **不能。** 要求保存的 LR=`40d07ba8`、完整 X0≤1、`[a03bd062c]≠0`。**真实调用点不保证 client 总是 0/1，见下文。** |
| `ready=0`、`claimed≠0`，或 claimed CAS 后复查失败 | **不能完全排除。** arm 时 claimed=0 不证明 REC 时仍为零；CAS 后失败仍消耗该 boot 的唯一会话。普通 Live 自动清 ready 的说法则不成立，见下文。 |
| 写锁失败 | **不能，但优先级低。** 单次 `LDAXR/STLXR`，锁占用或独占写失败均丢弃，置 `missed bit1`，不重试。准备成功发布 ready 后、释放锁前有小窗口；Live writer 使用的不是此锁。 |
| 已到 `record`，但尚未到 OPEN | **不能。** 包含快照、校验、cache clean/DSB、亮灯；快照确实会经 `encoder.hw` 读取动态 Live 结构，并非全部只读固定地址。 |
| OPEN／EOF SEEK／定位 SEEK／WRITE／CLOSE 同步挂起 | **全部不能。** 对应 RAM phase 为 1／3／5／11／13；准备事务成功不能证明 REC 任务上下文中的事务会成功。 |
| I/O 返回错误 | **不能。** OPEN 失败、EOF≠2048、定位失败、WRITE 错误/短写、CLOSE 错误均可使事务失败并清 ready，后续事件也不能落卡。 |
| 调用成功但内容未持久化，或复位中丢失 | **不能。** phase16 只证明 native 调用返回；当前没有可靠介质 flush 的证明。 |
| 任一阶段异常、栈不足、内存损坏或调度停止 | **不能。** armed 保存帧为 784 字节，另有 C/native 栈；没有现场 PC、任务栈余量或复位原因。 |

事件 2/13 依赖事件 1 发布的身份。因此，**三者全缺失不是三份独立的“未到达”证据**。

**【已证明｜Live 后是否会失效】**

1. **普通 Live finalize 不会直接清 ready 或增加 epoch。**  
   `live_finalize_wrap=401e4620` 更新 ticket size、释放 ticket lock，然后转发 stock。worker 的 job、writer、capture phase 只限制准备；同一 token 已 attempted 后，`fr_prepare` 直接返回，不会因拍 Live 再清 ready。

2. **确有“ready=1，但会话已经不可用”的设计缺陷。**  
   系统事件 24 在 `401e2180…218c` 增加 `runtime.card_epoch`；`fr_context` 在 `401e2414…2418` 因 epoch 不同拒绝，**不清 ready、不记录原因**。同一 token 又不会重新准备。shutdown 同样可造成这种失效。  
   此外，[arm.py](/path/to/Nikon/build-tools/livediag/feat-rec/recorder-owner/recorder/arm.py:37) 虽打印 recorder 的 epoch，却没有读取当前 runtime epoch 做比较。**这是诊断状态报告的缺陷；本次是否发生事件 24 尚无证据。** USB 断开事件 34 本身没有直接增加 epoch 的代码。[失效检查](/path/to/Nikon/build-tools/livediag/feat-rec/recorder-owner/runtime.c:31)

3. **client 限制存在明确覆盖缺口。**  
   MAIN `40d07b54…84` 实际逻辑是：

   ```text
   [x20+0x20] == 1 → client=0
   [x20+0x20] == 2 → client=1
   其他值          → 从 0xa031b6da0 读取已有 client
   ```

   随后仍在 `40d07ba4` 调用 bring-up；记录器在 `401f5e70` 比较 `X0<2`，拒绝其他 client。**不能拿 W5-BRINGUP.md 的“录像用 0/1”排除此路径；但本次 client>1 尚未实测。**

4. 事件 14 的异常回调确会清 ready，但必须先有 pending；pending 又依赖事件 1、4。**无此前 REC 身份、无内存破坏时，普通 Live 回调不能自行走这条清零路径。**

**【推测｜概率排序，仅作排查优先级】**

对“为何缺记录”，当前倾向：

1. 门控静默拒绝，首先查 client、epoch/shutdown。
2. recorder 在 REC 上下文中的同步 I/O 挂起或失败。
3. 本次 hand-back 未返回。
4. bring-up 之前阻塞，或探针自身异常。
5. 单次锁失败、纯持久化丢失。

没有足够样本给出数值概率。对**原始重启根因**，旧版也失败、无 Live 正常、电脑关闭后正常，使“Live→REC 交接或后续 stock 状态问题”仍优先；不能把新版 recorder 当作此前故障的唯一原因。18–27 秒也不足以证明 watchdog。

**【建议｜最小下一步，仅说明，不执行】**

信息量最大的无刷机实验是：**同一次复现，在重启前读取现有 RAM，连同已有 hand-back crumb/W5 RAM 日志一起读**，而不只看 ready。

- 必读：runtime `+0x6cc` shutdown、`+0x6d0` 当前 epoch；recorder `+0x5a90` 的 claimed/seen/missed/epoch/seq/tx；模式、`a031b6da0` client；已有 `EV_W5` client 和 W5 stage/fin_rc。
- `seen bit1=1`：hand-back 已返回且身份门控通过；再用 missed、tx phase 区分锁、I/O 区间、返回失败。
- `seen bit1=0`：用 epoch/shutdown、已有入口 crumb/client 和 W5 stage，继续区分会话失效、门控拒绝与 hand-back 未完成。
- 若要专门验证“拍 Live 即失效”，加一次 **Live 完成后、REC 前**的相同状态读取。USB 会改变生命周期，阴性结果不能排除脱线故障；这需要另行允许 USB，本轮未执行。

完全不使用 USB 时，卡灯只能辅助：长亮支持事务未结束，但灭灯不能排除未进入、短事务或 stock 覆盖。

若必须改固件，最小有效改动应是**记录原始入口 LR/client、hand-back 前后、拒绝原因及 I/O phase 的 RAM breadcrumb**。但当前没有核实到可安全指定的跨该重启保留地址：真实 `live_runtime_boot` 在 `401e2824…2830` 清零整个新 runtime，链接脚本也没有 retention 区。**不能把普通 RAM 或“空闲地址”宣称为重启后证据存储；必须先证明保留区及启动提取路径。**


[exited with code 0]
