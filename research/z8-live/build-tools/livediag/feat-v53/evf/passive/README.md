# v53 EVF 被动诊断候选（私有，未集成/未上机）

目的：下一次真机出现 EVF 没有 Live 图标时，记录显示路由、自己的 surface 状态和真实回调/合成调用计数。**没有修复已报告的 EVF 缺图标；没有改变 target、坐标、绘制内容或外部 root 所有权。** 原 G 的 Live On 原厂回放资源交接修复保持不变。

基线：`feat-v52/integration-g/work`，MAIN `ed1e8496fccde443412e527a252de5029a55c749053b9b046da42bfc454aa613`。仅修改 `changed-files.json` 中的 4 个私有文件；另须严格替换共享 core profile 的一个函数指针。`evf-passive.patch` 是完整源差异，`work` 在本目录的上一级。没有修改 root integration-c 或相机、USB、物理卡。

## 行为和发布

- `UiState` 尾部追加 448 B（37976 → 38424），Live runtime ABI 仍为 23264 B。没有可写静态对象，没有新的分配调用；使用原有 UI 对象那一次初始化分配。
- 原绘图回调只增加原子计数和最后结果记录；无文件 I/O、等待、分配。原有绘图/当前页/所有权判断不变。
- 新的 submit wrapper 只计数并用完全相同的 3 个参数调用原 `0x403efed8`，原样保留 X0 返回值。它记录的是**Fenestra 共享 profile 发起的实际提交调用**，包括可能的其他核心菜单使用者；不是所有原厂提交，也不能证明最终物理 EVF 扫描输出。
- 原 UI worker 每轮检查，只在 Live requested 且 EVF 时最多取 3 次快照（间隔至少 1 s），之后第一次回 LCD 取最后一次。早于 3 次就回 LCD 也结束；每次冷启动总计最多 4 次。EVF 三次用完不会继续读描述符。回 LCD 后保持不变。
- 每次快照由 UI worker 取得已有 GUI mutex；若取得锁期间输出切换则放弃该次。固定 native root、受限 RAM 指针、RAM descriptor getter 是唯一新增数据源；CNT 是时间源。**不读取任何显示 MMIO**。不跟随 `display.surface` 或 descriptor backing 指针。
- 回调计数器为原子；快照填完后 release 发布，消费者 acquire 读到完整记录。实际 ARM `stlr/ldar` 已核对。样本发布后不再改写；其余 used/drained 字段只有原 worker 使用。计数是采样瞬间值，不能当成跨线程同时发生的一帧事务。
- 原 worker 的 `log_step` 到达原有时间/5 s 速率门槛后，只有当前为 LCD、没有 stream job、writer、POST/FROZEN/ENCODING、快门 mailbox 或 twin-copy 时，才把样本加入原 crumbs 和 ring。回调不执行此步骤。没有新文件名，LLG2 结构、原文件操作和写卡失败重试行为不变。

## 原始字段和解释

每个样本 8 条事件 `61..68`，每条原 `live_ev` 为 24 B：时间戳为微秒，a 为第一个 32-bit word，b 低/高半各一个 word。总计 24 words：

1. 当前控制输出、Live status、attached/global-overlay/shutdown/loaded/blocked/gate/descriptor-enable/format。
2. 当前 native view 指针、id/state；自己的 display.surface 指针。
3. 静态 global-root id 和 group/output/target；其原厂 RAM descriptor 的 backing、width/height。
4. 绘图回调次数、最后绘图结果、共享 profile 的 native submit 调用次数。
5. display callbacks/error、display_status、present 成功/失败、not_current/target_lost。

绘图结果：`UINT32_MAX` 为 gate/current 检查未进入 paint，0 失败，1 成功，2 被阻挡。若回调次数为 0，最后结果未定义，解码器显示 `no_callback`。先看相邻 EVF 样本的计数差，再结合输出和 descriptor；首个 EVF 样本的累计次数可能来自先前 LCD。

`decode.py FILE [--output FILE.json]` 会从 crumbs/ring 合并一致副本；样本被覆盖时明确列出缺失事件，不补造字段。原 G 真机 LIVELOG/PREV 都可正常解码，但其中没有这些事件（`existing-card-logs.json`）。

## 日志保留边界

- 本诊断不是持续 recorder。一次启动的第一段符合条件的 EVF 使用会消耗采样名额；应先开 Live 并让启动超过 20 s，再触发所需 EVF 状态，保持约 3 s，回 LCD 空闲至少 5 s，然后尽快读取 LIVEDATA。
- 原文件格式容量不变：最多 32 条重要 crumbs、128 条 ring。4 个样本会占满 32 crumbs，并覆盖之前的旧 crumbs。以后新的重要事件仍可能逐步覆盖诊断；不能在长时间继续拍摄/菜单操作后保证它还在。
- 不回 LCD、没有到空闲/原日志时间门槛、worker/文件操作阻塞、写卡失败、记录后来被覆盖、连续冷启动或日志来自另一张卡，都可能没有完整记录。**缺文件或缺事件不证明 hook/callback 未执行。** 原 `LIVEPREV.BIN` 只保留原机制的一个前序日志，不是历史档案。
- 新 idle 检查与原日志 worker 一致，并不是全系统文件操作锁；不能保证检查之后另一原厂任务不会开始拍摄。没有引入新拍摄暂停或锁。

## 空间、验证和集成

实际完整链接（G 全部原对象，仅替换两编译对象）：

| 区域 | G | 本诊断 | 剩余 |
| --- | ---: | ---: | ---: |
| Live 主区 | 110528 B | 110592 B | 0 B |
| `.live_extra` | 2064 B | 3324 B | 236 B |

基线两对象以同编译参数重编，所有 alloc sections 字节与 G 对象一致；最终 ELF SHA256 `e26990cba830d27b60967ecbc948e5d6f673d4bb99f0e8dcf09eeb33ec86b1c5`。主区没有搬迁现有函数。**其他 v53 功能会争用空间，因此此候选仅私有保留，不能把本次 G 预算当成合并版本预算。** 必须对最终整合源重新完整构建/审查。

- `arm-result.json`：22 组实际链接 ARM 检查，通过。包含两条原回调 skip 路径、真实原厂 RAM getter、3+1 上限/节流/早返、GUI 取锁期间切换、各种忙状态、LCD 保存、每条 ring/crumb 字节和一次发布、非法/外部指针不读取 MMIO、全部 submit 参数/返回值和无 registry 透传。
- `host-result.json`：6 组主机对照，通过。实际 ARM 产生的 LLG2 ring/crumb 数据解码、丢记录反例、两份既有卡日志、严格 profile 指针及 4 个拒绝反例。
- `publication-proof.json` / `diag-disassembly.txt`：最终链接的发布/读取原子指令。
- `profile-result.json`：在 exact G MAIN 核对原 8 B 为 `d8fe3e4000000000`，替换仅落在该 8 B 范围。此内存测试未保存可运行 MAIN。
- 以上不是完整新 QEMU lane，也不是物理 EVF 可见性验证。先前 eye 诊断未复现真机缺图标，详见上一级 `INVESTIGATION.md`。

集成时源文件做三方合并，`runtime.c` 只涉及 `log_step`。先正常构建并 splice Live 主区/extra 和 hooks，**再运行** `profile_patch.py MAIN LIVE_ELF OUTPUT`（需要 pyelftools）。工具要求 `0x401def00` 原值严格等于 `0x403efed8`，`live_ui_evf_submit` 是合法对齐的 Live 地址，且传入 MAIN 的每个 ELF alloc section 与该 ELF 完全一致，拒绝输入输出同路径。它不会根据旧符号猜地址，也不会容忍已替换的指针。当前私有链接 wrapper 为 `0x401fda28`，这不是未来版本的固定地址。

可重跑脚本：`build_test.py` 使用隔离 `/path/to/user/fxfix/v53-evf/diag` 和 G 的只读对象；`test_arm.py` 使用同路径及 QEMU venv 中 Unicorn/Capstone/pyelftools；`test_host.py` 在本目录运行。`.elf` 仅供审查，不是刷机产物。
