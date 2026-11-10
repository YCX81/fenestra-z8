[codex] Turn completed.
**【已证明】run3 已到 hand-back 前的记录点；尚不能证明 hand-back 被调用或已经返回。“短暂闪两下”不能排除 a、b、c、d 中任何一项。**

本轮仅离线读文件、解码和反汇编，未修改文件，未使用 USB、相机、刷写或 QEMU。使用的是 `v3b/` 源码和产物；其 ELF 代码、只读数据与该目录 MAIN 一致。

1. **灯数与四个假设**

【已证明】记录器每笔事务只有一次显式 `lamp(1)`，位于 **快照构造、缓存清理之后，OPEN 之前**；事务返回成功或失败后执行 `lamp(0)`。正常完整 REC 共 **4 次置亮**：

| 顺序 | 事件 | 位置 |
|---|---:|---|
| ① | 15 | hand-back 前入口事务 |
| ② | 1 | hand-back 返回、门控通过、快照完成之后 |
| ③ | 2 | `0x402f303c` 服务返回事务 |
| ④ | 13 | adapter 完成通知调用返回后的事务 |

事件 16 最多额外增加一次；准备事件 0 在 REC 前，不计入。**这是 GPIO 置亮次数，不保证形成四次可见闪烁。** v3b 的 hand-back 本身没有旧版长亮/短码，内部 `w5_diag` 只写 RAM。[事务代码](/path/to/Nikon/build-tools/livediag/feat-rec/v3b/recorder-owner/recorder/recorder.c:88)、[交接入口](/path/to/Nikon/build-tools/livediag/feat-rec/v3b/recorder-owner/runtime.c:59)

【已证明】run3 后四扇区全零。事件 15 的 `[16,0,0,0,0,0]` 证明**准备事务已返回成功**，不证明入口事务自己的 WRITE/CLOSE 返回。入口 `x19=1` 是外层保存值；本次 client 是 `x0=0`。

| 假设 | “两闪”的支持或排除力度 |
|---|---|
| **a．卡在 hand-back 内** | **不能排除，也无新增支持。** 可以是入口事务＋原厂/其他流程的灯。甚至入口 CLOSE 尚未返回也符合文件现状。 |
| **b．hand-back 返回，事件 1 事务开始但未完成** | **兼容，只有很弱的支持。** 两闪可以对应入口＋事件 1；但第二闪也可能来自其他流程或事件 16。若灯没有其他控制者，事务内一直阻塞应持续亮；本次不能采用这个前提。 |
| **c．事件 1 写完但未持久化** | **兼容，无直接证据。** 灯灭只表示相关控制流执行过，不是介质提交确认。 |
| **d．事件 1 门控失败且没有拒绝记录** | **代码可达，不能排除；两闪不帮助区分。** 具体路径如下。 |

【已证明】d 有两类路径：

- hand-back 返回后，`fr_enabled` 或 `fr_probe` 再次获取 context；若 runtime 无效或 `arm=0`，直接退出，**不调用 reject**。
- 进入 `begin()` 后，ready、模式、shutdown、generation、claim 等拒绝都会尝试 `reject()`；但拒绝机会已消耗、写锁失败、I/O 失败/阻塞或未持久化，都可没有事件 16。**epoch 单独变化不拒绝；事件 1 也没有 client<2 门控。**

另一个容易混淆的路径是：事件 1 的 `persist()` 返回失败，只把 `ready=0`；末尾使用旧 `why`，若旧值为零，`reject(FR_AFTER)` 直接返回。这是 **I/O 失败无拒绝记录**，不是门控失败。[门控与拒绝代码](/path/to/Nikon/build-tools/livediag/feat-rec/v3b/recorder-owner/recorder/recorder.c:134)

【推测】c 的合理解释是：入口更早进入后台写回，或随后一次 OPEN/文件操作促使旧数据写回，而事件 1 尚留在缓存。**尚未证明 OPEN 有这种作用。** 两笔都是同一预分配文件的原位 512 B 写，没有证据说明入口天然更可靠；FS 数据、元数据和卡内缓存的提交次序仍未知。代码中的 `DC CVAC/DSB` 只解决内存可见性，不证明介质持久化。[格式与限制](/path/to/Nikon/build-tools/livediag/feat-rec/src-fix35/recorder/README.md:44)

2. **下一版最小诊断**

【推测／设计】优先选 **甲：保留入口事务，之后取消记录器文件 I/O及完整 Live 快照，用独立灯码取正向证据**。不改 close 算法和超时。

| 点 | 精确位置与动作 |
|---|---|
| **I：准备调用** | `fr_before` 返回后、`BL live_w5_bringup_notify` 前。区分入口事务尾部阻塞与已经准备交接。 |
| **R：已返回** | 紧接该 BL 后；当前链接地址 **`0x401e5488`**，必须在 `fr_enabled` 前输出，避免返回证据再受门控影响。 |
| **S：服务结果** | **`0x402f303c`**：先保存原始 **W0、W19**；W19 此时才是本次 client。 |
| **M：绑定后映射** | **`0x402f3068` 的 LDP 之前**：读取 `*(uint8_t *)(0xa02e98480 + W19)`。不能等恢复 x19 后再读。 |

【已证明】`303c` 在成功绑定之前，`rc=0、map=ff` 此时可以正常；`3068` 才是成功/失败汇合点。这里的 W0 已不是原始 service rc。[交接文档](/path/to/Nikon/build-tools/livediag/codex-w5/W5-MOVIE-HANDOFF.md:22)

【推测／设计】具体实现约束：

- 入口按完整 LR=`0x40d07ba8` 锁定一次会话、client 和原 SP；后续短码不依赖事件 1 的 `ready/claimed`，不跟随 `encoder.hw`。
- patch 使用 **B trampoline**；保存 x0–x30、NZCV、q0–q31、FPCR/FPSR，沿用 784 B 对齐保存帧。S 恢复原 W0 后重放 `CBZ`；M 重放 `LDP x19,x30,[sp,#0x70]`，续行 `0x402f306c`。
- 使用现有 GPIO 接口 `0x40c15c3c(0xa0280a1b0, level)`。

**灯码建议：**手机固定拍 **60 fps，优先 120 fps**。每帧采用“灭200 ms、亮250 ms、灭150 ms”同步头，随后 Manchester 编码：每半位50 ms，`亮灭=0、灭亮=1`；内容为 **阶段2位＋值3位＋CRC4**。I/R 值固定；S 的0–5原样编码，其余用逃逸码追加完整32位；M 编码0、1、ff，其余追加原始8位。这样零值也有完整可见帧，普通约1秒长亮不能直接充当有效码。

每个普通帧约 **1.5秒**，四点合计约 **6秒额外等待**。计时器定截止时间、以现有 `DELAY(1)` 让出任务，避免忙等饿死 worker；调度仍可能拉长脉冲。**这些延时会改变竞争与超时条件。** 原厂灯干扰造成的非法脉冲或 CRC 错误整帧作废；必要重发放在 M 取样之后。**缺码仍不能证明没返回；有效 R/S/M 才是到达的正证据。**

【推测／设计】乙若保留写卡，最值得比较的是：三点仅存固定 RAM，**M 取样后单笔写出累计的 R、原始 W0、client、map、校验**，取消每点重新 OPEN 和完整快照。这减少干扰，但若到不了 M，就没有文件返回证据；若逐点累计写，又恢复 FS 阻塞歧义。加长写入、重复 OPEN、读回或 CLOSE 均不能在没有已验证 flush 语义时保证持久化。因此本轮目标更适合甲。[REVIEW5 的不可区分反例](/path/to/Nikon/build-tools/livediag/feat-rec/REVIEW5-RESULT.md:17)

3. **取得返回值后的修复方向**

下表左栏是已核对的服务分支；修复方向均为【推测】，尚非现场根因证明。

| `303c` 的 rc | 【已证明】含义 | 【推测】修复方向 |
|---|---|---|
| **2** | slot 已满或所需核心忙，先于 arena 比较 | 查未释放资源及其所有者；完成受序列化保护的交接，不能直接清占用标志。 |
| **4** | 参数检查失败，或服务仍启动且 arena/size 不匹配 | 先区分参数错误与服务残留；不能仅凭4认定 Live arena 冲突。 |
| **5** | 冷初始化返回−3；握手等待失败可产生−3，且该失败分支未撤销新 slot/core | 查 worker 应答及退出/重新上电顺序；不能只延长等待或盲目重试。 |
| **0** | service 分配成功 | 若 M 为0/1，转查 adapter OPEN、CREATE、HEADERS；若 M 为ff，查绑定及并发清映射。0不证明录像成功。 |

**【已证明】只让 REC hook 返回失败不够：`0x40d07ba8` 覆盖返回值，随后仍调用 adapter。** 可返回的失败路径必须同时修改调用者：交接失败时跳过新 bring-up/adapter，并走经过核实的 MOVRS 失败收尾；当前还没有证据支持随便选一个跳转地址。[错误传播](/path/to/Nikon/build-tools/livediag/codex-w5/history/astra-bbzen9eh9.md:64)

【推测】长期应把 close 放到**同代照片通道成功停稳、旧缓冲尚有效、Wave5 worker仍能推进**的窗口，REC 只检查交接完成；失败可返回的保护仍必须保留。**不能直接恢复“模式变了就 close”**：文档明确 `0x40e91078` 只是候选观测点，尚未证明能在其持锁上下文安全执行 EXIT，也未证明它早于旧缓冲复用。[停稳判据](/path/to/Nikon/build-tools/livediag/codex-w5/W5-MOVIE-HANDOFF.md:26)


[exited with code 0]
