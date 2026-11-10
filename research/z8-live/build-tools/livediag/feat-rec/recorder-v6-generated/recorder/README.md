# lamp-v4 + skip_nr：仅 Live 关闭的 NR 复位实验

这是待复审的本地实验实现，不是故障修复或上机批准。未使用 USB、刷写、操作相机或 QEMU；指令级测试使用 Unicorn，原生调用被桩替代，没有执行 NR MMIO。

输入为本地 `src-fix35/recorder_patch.py`、`src-fix35/recorder/`，同步 overlay 为 `recorder-build/`。保留旧 `linked-lamp-v4/`；新链接在 `recorder-build/linked-skip-nr/`，仍标记 `flashable=false`。旧说明存于 `linked-lamp-v4/README-baseline.md`。

## 来源与假设

只读阅读了：

- `../codex-w5/qemu-handoff-20261110/README.md`
- `../codex-w5/qemu-nr-rec-20261110/README.md`
- `../codex-w5/qemu-rec-after-open-20261110/README.md`
- `../codex-w5/history/rea-153f67f-ack8-nr/report.md`

这些报告记载 v3b 在修正后的模型中 S=0、OPEN(0)、产生可解码 MOV；旧模型拒绝的是 EXIT 对 NR 的两笔复位。**取景活动时复位 NR 导致真机看门狗重启仍是假设**，不能从模型拒绝推成硬件因果。

实验室 MAIN `5c29c9ee99fe18450386e0cb1e5f6a3b1207bff0bea501c26ae4b32aa916d91e` 由最后一份报告给出；本地原 lamp-v4 MAIN 实际为 `53eaa9767a3f4ac24846b5a1c1c2eae2ee4abdcfe5d63ea87ef1445381f73275`。本次重建使用锁定的 `bg34.main` (`c7c6dbee…`) 和本地源码，**未拿到实验室 MAIN 本体并完成逐字一致性证明**。不能宣称这是对 5c29c9ee 二进制的直接补丁。全量链接显式使用 `compile-defines-v4.json=[]`；并未恢复缺失 owner relive 脚本的外部宏。

## RAM ABI 与默认关闭

沿用保留字，runtime/recorder 大小与原门控完全不变：

|字段|fr_state 偏移|意义|
|---|---:|---|
|arm|0|原灯码 token，0 禁用|
|lamp_seen|52|低4位仍为 I/R/S/M；bit8/9 记录实际跳过的 4a/4b|
|live_exit_active|88，8字节|0=无所有者；非零=当前私有 EXIT 的精确 SP|
|entry_claimed|96|原单次入口领取字|
|skip_nr|100，4字节|只有精确值1启用；其他值拒绝|
|tx|104|原事务区，不移动|

新启动 runtime 整体清零。`arm.py --token N` 默认明确写 skip_nr=0；`--token N --skip-nr` 才写1；`--skip-nr` 不允许无 token 或 token=0。取消 arm 同时清 skip_nr。原生关闭、未 armed、skip_nr=0/非法值均不会跳过。单独写 skip_nr 而不 arm 无效。

工具发布顺序为 arm=0 → skip_nr=0/1并读回 → arm=token并读回。两个字段不相邻，准备发布时可能短暂出现 arm=0/skip_nr=1，**此时有效实验开关仍为关闭**；不要把原始配置字解释成独立于 arm 的生效开关。工具拒绝 claimed、lock、entry_claimed 或 live_exit_active 非零时的写入，避免一对复位之间改配置；一组实验后重启再进行下一组，不在已领取会话内切开关。工具不提供原生任务并发写入的原子协议，必须在空闲照片模式准备，不能同时由其他写入者改 RAM。

layout 对象追加 skip/active 偏移和 `NR01` 标识；旧 layout 对象不能用于新写入接口。runtime 大小未改变，因此 layout 标识不能单独证明正在运行的新镜像，必须核对构建身份。

## EXIT 隔离、调用栈与并发

**没有在原厂 0x402f3184/3190 或 0x40c2d884 上放置全局门控。** 原厂 EXIT `0x402f3084..0x402f3258` 完整保留。原厂自身录像结束无论是否与 Live 并发，执行的都是原字节，仍调用全部 0x43–0x4b。

仅 overlay 的 `w5_close_body` 将 `EXIT(W5_CLIENT)` 改为 `fr_live_exit(W5_CLIENT)`；STOP/TRIGGER/FINISH、轮询、超时和 close_lock 不变。`fr_live_exit` 在未 armed、skip_nr!=1 或 client!=4 时调用原地址 `0x402f3084`。启用时调用 `fr_exit_scoped`：

1. wrapper 保存 X22/X30，X22 持有 fr_state；CAS 把 `live_exit_active` 从0变为预期 EXIT SP。冲突不等待，直接回落原厂 EXIT，不清除别人的标记。
2. 运行 **原 EXIT 前段的私有重定位副本**（原地址 3084..3190），随后 B 回原厂 `0x402f3194` 执行完整尾段。这是一项有意的实现选择：启用分支没有再次进入原地址3084，避免给所有原厂 EXIT 新增门控。副本逐条核对锁定原字节，BL 保留原目标，CBZ 保留原内部目标；其他未知 PC-relative 指令拒绝构建。
3. 只有副本对应 3184/3190 的两处调用有守卫。要求 arm!=0、skip_nr==1、活动 SP 等于当前 SP；先比较 SP 再读取原 EXIT 栈 `SP+24`，必须是完整 `fr_exit_return`。保存的 X30 必须分别等于副本上一调用返回位置 `fr_nr_lr_a` / `fr_nr_lr_b`，且 W1 精确为 4a / 4b。任何不符均重放原 BL `0x40c2d884`。
4. 每处守卫沿用 lamp-v4 `fr_save/fr_restore` 的784字节完整帧：X0–X30、NZCV、Q0–Q31、FPCR/FPSR。跳过后保留参数及状态；第一处汇合后将 X30 设为本次等价调用的续行地址，供第二处核对。通过守卫才置 lamp_seen 对应证据位，无 I/O、GPIO、DELAY 或 NR MMIO。
5. 原尾段恢复32字节 EXIT 栈，并尾调用原 `0x40c2fb80`；返回 wrapper 后才清活动标记，保留 EXIT 返回值并恢复 X22/X30。若 EXIT 卡死，标记保持，后续竞争者保守回落，不能冒领。无取消/长跳转恢复保证。

并发安全不依赖一个全局布尔值：原厂路径完全不经过守卫；即使另一个私有调用进入也必须先取得 CAS 所有权，SP、栈 LR、即时 LR 三者匹配。既有 Live close_lock 仍保留。测试中的“并发原厂”是持有活动标记时运行原厂指令的反例，不是原生调度器并发证明。

现有 I/S/M 门控源码与对象前段不变，未 armed 的原路径仍为111条指令/32字节 scratch。**Live 的关闭处增加了一次选择调用和状态查询；不能声称整个关闭路径零额外成本。** 不修改原门控也不等于所有路径逐周期相同。新 wrapper 和 NR 保存帧增加的栈深度尚无真机余量证明。

## R 灯码与既有协议

R 的3位值码从固定000扩为000..011：0=未观察到跳过，1=仅4a，2=仅4b，3=两次均跳过。成功取到 REC 会话时清除旧 NR 证据位，因此不会把本次入口之前完成的跳过当成当前 R 证据。若 worker 正在关闭，期间实际通过守卫的调用可计入本次 R。R=3只证明这两次调用被绕过，不证明 NR 正常、close 成功或原厂初始化成功；R=0不能证明开关未设置（Live 可能已提前关闭）。缺少有效 CRC 的 R 也不作否定证据。

同步头灭200/亮250/灭150ms，Manchester 半位50ms，阶段2位＋值码3位＋CRC4（多项式 x^4+x+1），扩展规则不变：S 的0..5直接编码，其余110+32位；M 的0/1/ff分别为0/1/2，其余110+8位。I仍固定0。新解码器支持 R=0..3，旧解码器会拒绝非零 R，必须更新视频工具。没有增加帧长或新的灯码点：四个普通帧名义6秒；两种逃逸均使用时10秒，加上实际调度迟滞。

原文件协议仍为 WFL4、3072字节，仅准备与入口各写512字节记录（准备预分配3072）；I/R/S/M 之后不增加文件事务。S是原始W0，M是汇合点client映射；这些都不是最终录像成功证明。取景灯的物理争用、曝光、真实 DELAY 量子/最长延迟未验证。

## 解除复位证据及风险

【本地基底反汇编/表数据】

- EXIT 的3184/3190原 BL均到 `0x40c2d884`，W1为4a/4b。
- 查表 `0x4164c494 + 2*ID` 的两项为 `(8,23)`、`(8,24)`。置位函数在 `0x40c2d8c4` 写 `0x0d154348`，值分别00800000/01000000。
- bring-up 在 `0x402f2f44`、`0x402f2f50` 以4a/4b调用 **`0x40c2d8cc`**；`0x402f2ed8`附近本身是时钟配置序列，并非这两次解除。
- 解除函数使用同一张表，在 `0x40c2d90c` 写 **`0x0d154248`** 同样两掩码。没有读取旧锁存位、条件等待、检测先前置位成功或额外软件状态转换。以上原字节全部不改。

【推断】若该端口遵循置位/清除锁存语义，向已经解除的位再写 clear 应是幂等的；从这段软件没有看到依赖“先 set”的控制流。但没有硬件手册或本次真机读数证明此写无副作用，更不能由清位幂等推导出 NR 初始化安全。

**主要未验证风险：跳过置位可能保留旧 NR 配置、在途包、事件或内部状态；随后原厂 bring-up 的初始化可能依赖复位脉冲，即使 clear 写无害也可能挂起、出错或录像损坏。** 也未证明 USB 暂停取景就解释了全部真机差异。本次不读取危险的154x寄存器，不用软件模拟结果替代硬件证据。

## A/B 上机对照草案（本次没有执行）

前提是另行完成镜像来源、复审与上机授权，并有可用的**非 USB 原有 RAM arm 入口**。本次仅提供离线写入计划，不安装或执行相机侧 arm 服务；若没有该入口，在当前禁止 USB 的约束下不能完成武装，不能把未武装运行当 A 组。

先离线查看新 `linked-skip-nr/layout.o`。以下命令只输出字段和有序计划，**不连接设备、不写 RAM**：

```sh
# A：只武装灯码，明确 skip_nr=0
.recorder-venv/bin/python src-fix35/recorder/arm.py \
  recorder-build/linked-skip-nr/layout.o --plan --token 101
# B：相同灯码，增加 skip_nr=1
.recorder-venv/bin/python src-fix35/recorder/arm.py \
  recorder-build/linked-skip-nr/layout.o --plan --token 102 --skip-nr
```

1. 每组独立冷启动，使用相同设置、卡和场景；照片模式空闲时，由已有非USB arm机制执行对应计划并读回。不得让入口claim、prepare锁或EXIT活动时写入；token对应文件不得已存在。等待准备完成，记录 ready/失败情况。
2. A：开启Live并确认活动取景，拍照，采用固定等待切录像档、第一次REC。外部视频连续覆盖准备灯与I/R/S/M；预期R=0。保存录像成败、黑屏/重启、各有效灯码与时间。
3. B：重复相同步骤和等待，使用正token＋skip_nr；若关闭确实在本次窗口发生，R=3为有效实验执行证据。R=1/2属部分跳过，要保留并调查；无R或R=0不能当作B已生效。
4. 若能开始录像，再观察停止及文件可解码性。原厂停止的EXIT仍包括全部NR复位；“只在停止时挂”需要单独记录，不能认为本实验已覆盖。A/B均承受原灯码等待，不与未武装速度直接比较。一次B成功不能单独证明NR是真机重启原因。

现有 `--usb --helpers ... --token ... [--skip-nr]` 接口同步支持新配置，但本任务没有运行且上述对照禁止使用它。

## 本地复跑与验证边界

```sh
# 输出必须为新目录；不要运行旧 build.py
.recorder-venv/bin/python src-fix35/recorder_patch.py --out recorder-output --check
ZIG_GLOBAL_CACHE_DIR=/tmp/feat-rec-zig-cache .recorder-venv/bin/python \
  src-fix35/recorder/relive_recorder.py recorder-build bg34.main bg34.changed-ranges.json \
  recorder-build/linked-skip-nr-new --defines-json recorder-build/compile-defines-v4.json \
  --zig .recorder-venv/lib/python3.14/site-packages/ziglang/zig
.recorder-venv/bin/python src-fix35/recorder/run_checks.py recorder-build recorder-build/linked-skip-nr
```

汇总位于 `recorder-build/recorder-check/skip-nr-tests/results.json`；`linked-skip-nr/nr-audit.json` 保存新测试，`lamp-audit.json` 保存灯码位置，`link-check.json` 保留完整验证器原结果。完整 `verify_link.py` 未修改、未扩大CFG；旧forwarding证明不完整仍必须报告FAIL，统一命令因此返回1。具体通过数量及MAIN哈希见这些机器可读文件，不以本README替代实际产物证据。

本次最终结果：10项通过（含新NR的288条守卫路径、6条完整作用域路径），1项失败（完整verify_link的旧CFG限制）。视频测试曾因更新R扩展用例时沿用了旧波形夹具失败，重新运行生产C发码生成夹具后通过，未改变解码容差或跳过断言。新增NR反汇编和掩码证据在 `linked-skip-nr/nr-disassembly.txt`。

本次实验 MAIN SHA256：`d2504dfbc8e2365faa2c0c2b4582cdfeaa80fd16730cf2bcce078252bf12e66c`。硬件、USB、刷写、QEMU均未执行；Unicorn只验证寄存器/控制流，不验证NR硬件、原生锁与调度或录像初始化。
