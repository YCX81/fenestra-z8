# v35 recorder 修订：主动 arm、单 REC 会话、三次落卡

这是诊断源码及本地检查结果，不是已验收固件。完整阅读 `REVIEW-RESULT.md` 后修订。没有连接 USB、刷机、操作相机或进行硬件测试；没有完成真实全量链接。原 hand-back 函数体逐字保留，STOP/TRIGGER/FINISH/EXIT、原超时及 stock 返回值不改。

## 启用与启动隔离

`fr_state.arm` 随 runtime 分配清零。固件代码从不将它置为非零。未 arm 时，worker 不调用准备函数；新增探针先用汇编只读查找 UI 注册链和 arm，仅保存 x9–x12（32 字节）并在 x11 中保存 NZCV。每层指针在解引用前检查对齐、RAM 范围及读取范围不越界，并检查 core/registry/UI/runtime 的身份；不调用 C、不保存 SIMD、不读计时器、不写共享 RAM、不做 native I/O。runtime 大小与 recorder 偏移由编译断言固定；变更 layout 必须更新并重新验证门控。

编译对象拒绝路径实测：完整有效链但 arm=0 为 **111 条指令、32 字节峰值栈**；测试中的空指针／越界／未对齐／未初始化身份拒绝为 **14–107 条、32 字节**。从 trampoline 入口计到 stock 恢复点之前，包含门控分支，不含 patch-site 的 B、被覆盖指令和 stock continuation。只恢复 SP、原寄存器、NZCV 及调用者有效栈；SP 以下已释放的 32 字节保存区不抹回旧字节。栈写入共32字节、读出32字节。

共享 bring-up 拒绝后仍执行原 hand-back，原有整数帧256字节和 C 深层栈不计入上述 recorder 门控成本；两个帧不叠加。已 armed 路径在一个784字节完整帧内完成 hand-back、资格检查及 recorder，不再重复保存整组整数寄存器。

准备只在 worker 看到非零新 token、PHOTO、LA_READY、无 writer/job、未 shutdown 时进行，并在取锁后复查。模式仍可能在复查后改变；arm 后的准备有同步 I/O 风险。这里的“无 boot I/O”仅指 recorder 新增操作；不宣称原 Live/stock 固件完全没有文件操作。

arm token 为 1..99999。文件名为 `A:\W5F00001.BIN`（token=1，以此类推），不是旧版 W5FR.BIN。worker 使用排他创建 `0x101`，写 2048 字节并 CLOSE。文件存在、无卡、卡满、短写、CLOSE 返回错误：ready=0，本 token 不自动重试，不覆盖/截短/删除已有文件。用户可选更大的 token，在照片待机重试；部分创建文件也保留。已有旧文件与新 token 的编号是否冲突，由排他 OPEN 决定。即使排他失败，OPEN 自身仍可能挂起。

准备成功后 arm 必须保持该 token；用户确认 ready=1 再按 REC。一个 boot 只接受一个精确 REC 调用。`claimed` 不复位；发生 REC 后，无论成功、失败、切模式或改 token，都不允许第二个 REC 会话/再准备。这样旧任务不能混入后续代次。准备失败后的重试仅限尚未 claimed。写 0 可禁止后续新事务，不能撤销已经进入 native 的调用，不能强解锁。挂起时不会由重试启动第二个事务。

### RAM 地址与工具（此轮仅离线检查，未执行 USB 模式）

地址链来自 `ui/live_ui.c` 和 `../livectl.py`，不使用 livectl 的旧固定 runtime-size 表：

```
root = *(u64 *)(0xa032b50c8 + 16)
core = *(u64 *)(root + 663*96 + 80)
reg  = *(u64 *)(core + 2528)
ui   = *(u64 *)(reg + 8)
runtime = *(u64 *)(ui + 8)
arm_address = runtime + offsetof(struct live_runtime, recorder) + 0
```

`--check` 用 **AArch64 编译器**生成 `recorder-check/layout.o`，`arm.py` 从 ELF 读取 sizeof/offsetof；不猜地址，也不采用 host ABI。所有者全量构建后应改用 `linked-recorder/layout.o`（与实际全量编译参数一致）。当前结果：runtime 大小25424，recorder偏移23184 (`0x5a90`)，tx偏移96。工具检查 core/UI/runtime magic、runtime大小、claimed/lock、照片模式和 token 递增。绝对地址随 boot 分配改变，不能把某次结果硬编码。

离线显示布局（不会导入 USB 模块）：

```sh
.recorder-venv/bin/python src-fix35/recorder/arm.py recorder-build/recorder-check/layout.o
```

仅供所有者将来另行决定硬件实验时使用，以下命令本轮**没有执行**：

```sh
# helpers 指向有 livectl.py / z8_audio_probe.py 的目录；运行环境需其 USB 依赖
python recorder-build/recorder/arm.py recorder-build/recorder-check/layout.o --usb --helpers ../ --token 1
python recorder-build/recorder/arm.py recorder-build/recorder-check/layout.o --usb --helpers ../
```

状态前16字依次为 arm、attempted、ready、claimed、lock、generation、seen、missed、client、kbps、size、epoch、seq、pending、adapter_gen、reserved。工具还读四项 tx。USB 本身会改变相机生命周期，读回不是无扰动观测；工具不能证明相机上的 payload 与本地 ELF 完全相同。

## 身份、并发与落卡点

|事件|地址／基线原字节|作用|
|---|---|---|
|1|402f2d58 / 0fc9fb17（既有B）|原 hand-back 返回后，精确 LR=40d07ba8、client=0/1、非照片模式；CAS claimed，发布本次身份；落卡|
|2|402f303c / 00010034|service-open rc；验证 client=w19、当前栈=REC入口栈−0x80、栈内保存LR=40d07ba8、当前LR=402f303c、generation；落卡|
|4|402f5870 / f37bbfa9|adapter启动入口：LR=40d07bb0 且与REC同一调用栈；发布 pending generation；仅RAM|
|14|402f4560 / ff8306d1|adapter回调入口：携带启动位就进入原子消费；仅精确事件1且符合身份/模式时绑定栈与generation，其余置DONE并撤销ready/adapter_gen；仅RAM|
|13|402f4c1c / 53050836|完成通知调用40d2bc8c已返回；验证回调栈−0x1a0、w19 bit0、LR及generation；落卡；恢复原TBZ|
|5|402f58cc / e1030032|MOVRS等待调用已返回，核对当前栈、保存的MOVRS LR和当前LR；仅RAM|

以生成 manifest 的实测原字节为准。事件13到达说明 adapter 的 **REC启动分支完成通知调用返回**，不等于整个常驻任务退出、通知成功、MOVRS已醒来或视频已录制。它也可能走“adapter原已非零状态，直接通知”路径；记录 adapter 状态用于辨认，不能把它说成全套初始化成功。

普通新增探针的身份核对在SIMD保存前执行；共享 bring-up 在 arm 非零后复用 hand-back 的完整保存帧。`seen` 用 atomic fetch-or，写锁只保护缓冲与文件事务。事件5与13任意顺序/同时到达都会置 DONE；即使 ready=0 或写锁被占用，生命周期仍更新。争用时 atomic missed 记录丢写。身份字段先写后用 release 发布，读者 acquire；准备代次/卡代次亦为原子访问。锁只尝试一次，不等待。原生 I/O 本身没有可证的有限延迟。

正常照片/播放调用没有精确 REC LR、client、栈/代次链，不会抓快照或写卡。stock 的异步事件没有可携带的自定义 cookie，因此 adapter关联仍依赖 stock 事件1由上述REC启动发布、回调串行消费的协议；混合事件参数会原子作废 pending 并终止本 boot 的诊断会话，后续 REC 不得复用其身份。若 stock 已有残留/合并事件或任意内存损坏，不能单靠软件 token 证明底层事件的因果来源。一个boot一次REC、照片模式拒绝、卡代次/关机检查及精确栈/LR使旧版active残留不再单独允许共享路径写卡。

## 事务进度与证据边界

四项 RAM `tx[4]` 分别对应准备及最多三笔 REC 写入，每项六个u32：event、phase、error、written、close_rc、serial。每个 native 调用前后发布 phase：

|phase|含义|
|---|---|
|1 / 2|OPEN 即将调用 / 已返回|
|3 / 4|SEEK EOF 即将调用 / 已返回|
|5 / 6|SEEK offset 即将调用 / 已返回|
|11 / 12|WRITE 即将调用 / 已返回|
|13 / 14|CLOSE 即将调用 / 已返回|
|16 / 18|整个事务调用返回成功 / 返回失败|

单个 phase 用 release store；其它字段跨多次 load，不是原子快照。奇数只把停顿定位到“标记后至返回标记前”，包含调度停顿，不能证明 CPU 正在该函数内部。文件带有**先前事务**的 phase；本笔 WRITE前标记不是成功返回证据。最后一笔的 WRITE/CLOSE结果仅在RAM；复位会丢失。不开额外文件写进度日志，避免用另一个同样可能挂的 I/O 证明前一个。

卡灯在整个事务前亮、完成/返回失败后灭，不加闪烁延迟。这样现场长亮可辅助指向 recorder I/O；每阶段闪码既需要延迟又与stock共用GPIO，不增加可靠区分力，未采用。灯可被stock覆盖，短事务可能不可见，GPIO调用本身也处于标记附近。**只有离线文件和共享灯，仍不能可靠区分下一次 recorder OPEN/SEEK/WRITE/CLOSE 卡住与下一 stock 步骤卡住。** 活着的RAM phase提供更细区间；不承诺挂起时USB可用或读RAM无扰动。

REC成功路径仅3次512字节 WRITE（原12次减至3次），每笔 OPEN、EOF SEEK、offset SEEK、WRITE、CLOSE；准备另1次2048字节 WRITE。少一个落卡点就会失去 hand-back、service rc 或完成返回三者之一。未增加“每阶段持久化”写。落卡失败不重试该REC记录；三笔中后笔缺失并不能反证事件没有发生。

## 文件格式与判定表

v2魔数 `0x32524657`，2048字节，四个512字节扇区；序号=物理扇区，字127为前508字节FNV-1a。0准备，之后1/2/13（争用可能缺记录）。`decoder.py`只接受v2；旧版W5FR文件不能用新格式解释。

字0..17：魔数、版本、序号、事件、site、计时器、boot准备计数器、client、w0、x0/x19/LR；18..20为准备事务错误/字节/close；21卡代次；22..25请求kbps/size/arena；26 missed，27 seen。28..117沿用原Live/Wave5固定RAM快照（不跟随动态指针、不读VPU MMIO）：Live状态、worker、slot、core、service arena/size、VDI、client映射、实例槽、adapter状态和模式。118 generation；119..122四笔事务phase；123当前事务号；124当前写前标记。准备session计数器不保证跨重启唯一。快照不是持锁原子快照。

|观测|可支持的结论与限制|
|---|---|
|未arm，没有新文件|预期行为；不能推断任何REC步骤|
|arm后没有文件/只有部分文件|prepare未调度、排他拒绝、无卡/错误、native挂起或未持久化；看RAM attempted/ready/tx0|
|只有准备记录|不能断言hand-back未到；可能门控拒绝、事务挂起或未持久化|
|事件1资源已释放|hand-back后的软件状态样本；不能证明VPU健康或本次close完全成功|
|事件1 svc_up仍在、arena/size不同|拒绝路径候选；需结合事件2，不能单凭此命名根因|
|事件2 rc=2/4/5|资源拒绝/参数拒绝/初始化映射候选；rc4不等于必然arena不匹配|
|事件2 rc=0|service-open返回成功；不是adapter或录制成功|
|事件13|完成通知调用已经返回；不是发送成功、任务退出或MOVRS唤醒保证|
|seen有5和13，DONE=1|两个返回探针已到达；写锁争用不阻止结束|
|missed有位|该身份校验通过的事件到达过，但没有落卡；末笔missed可能只在RAM|
|tx为奇数且稳定|recorder标记后的native调用区间或调度停顿候选；不能再归因于下一stock步骤|
|tx=16且无下一事件|上一事务全部调用已返回；下一stock区间/下次探针前区间候选；非持久化证明|
|灯长亮，只有上一条文件记录|支持recorder事务未完的可能性；stock覆盖灯、未落卡和调度仍不排除|
|复位后只有文件|最后一条不证明其CLOSE返回；不能确定watchdog、VPU hung或具体挂点|

## 离线复现与完整链接

从 feat-rec 执行；新输出目录必须不存在：

```sh
python3 src-fix35/recorder_patch.py --out recorder-output --check
.recorder-venv/bin/python src-fix35/recorder/verify_static.py recorder-output
python3 src-fix35/recorder/test_io.py recorder-output
.recorder-venv/bin/python src-fix35/recorder/test_gate.py recorder-output
.recorder-venv/bin/python src-fix35/recorder/test_recorder.py recorder-output
.recorder-venv/bin/python src-fix35/recorder/test_verify_link.py recorder-output
```

本次已用源默认宏编译通过全部19个AArch64单元；尚未全量链接。`verify_static`只检查基线指令/目标、hand-back源码不变和对象原子指令；**不再称作最终relocations PASS**。host测试用stub替换native、RAM和写锁LL/SC；用真实host原子操作与pthread交错检查会话结束，不证明ARM硬件/stock调度正确。门控另外通过限定 A64 指令子集解释器执行编译对象中的3360条测试路径（6入口×35条件×16种NZCV），覆盖空指针、边界、未对齐、身份错误、零arm及非零arm入口；未知指令即失败。共享 bring-up 复用帧做对象结构检查。解释器不是硬件执行，亦未运行完整payload。尝试 Unicorn 时本机在 mem_map 阶段 SIGILL，未执行门控，不能算作通过。

本机没有 relive_w5.py / ld.lld / llvm-objcopy；已读 `../v35_patch.py`、`../../w5/setup_src.sh`、`../../w5/coexist_patch.py` 和 `../chain.sh`。不使用旧build.py（源列表与上限均不适用）。提供 `relive_recorder.py` 作为 **本诊断构建专用的 relive_w5 替代入口**，采用相同 `SRC IMAGE MANIFEST OUT` 参数和 `live-fixed.emulation-only.main`/`changed-ranges.json` 输出供chain消费。它全量重编19个单元，追加recorder，复用manifest原始core目标，重定向全部15个旧Live hook和5个新增探针。输入必须是bg34及与它SHA一致的manifest；不接受拿旧manifest碰运气。读取所有者 `relive_w5.py` 中的字面 `-DLA_*` / `-DLIVE_*` 编译参数，避免静默改变几何/码率。若参数由动态表达式产生或同一宏有冲突，适配器拒绝运行，需先把实际参数审阅为字面值；它不会执行或修改原relive脚本。缺符号或未知布局直接失败。

所有者在有工具链的机器上，将此整个目录放入现有 live-audio 工作目录的 `feat-rec`；把**对应bg34的** changed-ranges.json 放为 `feat-rec/bg34.changed-ranges.json`，执行：

```sh
L="$HOME/.local/share/nikon-z8-qemu/live-audio"
cd "$L/feat-rec"
PY="$HOME/.local/share/nikon-z8-qemu/venv/bin/python"
# PY需pyelftools、capstone；PATH需clang、ld.lld、llvm-objcopy
python3 src-fix35/recorder_patch.py --out recorder-owner --check
"$PY" src-fix35/recorder/relive_recorder.py recorder-owner bg34.main bg34.changed-ranges.json linked-recorder --relive "$L/relive_w5.py"
"$PY" src-fix35/recorder/verify_link.py --base bg34.main --final linked-recorder/live-fixed.emulation-only.main --elf linked-recorder/payload.elf --hooks linked-recorder/objects/hooks.S.o --src recorder-owner > linked-recorder/final-check.json
```

这里对bg34已组成的播放/USB/pool层逐字保留，通常无须重复chain。若按现有流程重建其它层（仅离线，**不是刷写命令**）：

```sh
cd "$L"
bash chain.sh feat-rec/linked-recorder feat-rec/chain-recorder
cmp feat-rec/linked-recorder/live-fixed.emulation-only.main feat-rec/chain-recorder/pool/pool.main
```

`cmp`不相同就不得沿用此前PASS：chain可能使用不同播放源码或改动保留区。需独立审查其它层变更，当前checker不会放宽白名单替其背书。chain自身cd到live-audio，传上面相对路径保证输出仍在feat-rec。

`verify_link.py`检查：固定基线SHA、长度、每个ALLOC ELF段与MAIN一致、无可写段/未定义符号、范围 `[0x401e2000,0x401fff80)`、15旧+5新B/BL到最终符号、独立固定的全部6个旧continuation及6个旧转发符号（核对SHA固定基线的分支证据），9个旧转发wrapper的实际目标（包括3个C绝对调用）、对完整hooks.S对象应用真实重定位后与ELF/MAIN逐字一致、全镜像差异白名单。它不是CFG/间接引用穷尽证明。

另发现bg34在 `0x401fe000` 和 `0x401fe070` 有core辅助代码，分别由 `0x401cf98c` B、`0x401d8984` BL引用。该段不属于Live重新链接对象。额外保留 `[0x401fe000,0x401fff80)` 字节和这两个入口，实际新Live链接上限更严格为 `0x401fe000`；原USB尾部 `[0x401fff80,... )` 也不能碰。工具不会为腾空间覆盖它们。全范围原始字按B/BL解码会把固件数据误报成分支，不能据此声称找全代码引用。

## 尚未验收的风险

同步OPEN/SEEK/WRITE/CLOSE取stock FS锁，不能保证退出上界、无死锁或介质flush；本实现可能制造被测停顿。普通激活探针SIMD+整数保存784字节；C `.su`仅局部栈用量，native深层调用、任务栈余量、FP权限、DMA/cache路径仍未认证。缓存别名与clean方式沿用原native包装。卡拔出/模式切换与单次I/O的竞态仍在。完整链接检查即使通过，也不等于硬件安全或允许刷写。


## REVIEW2 修复复验

三项修复均已同步到 recorder-build：启动位回调不可在外层门控遗留 pending；旧返回/转发目标独立固定；零 arm 轻量安全查找与共享 bring-up 单帧。`test_io.py` 新增合并事件3 → 等待返回 → 后一次REC的回归，包含照片模式拒绝、ready=0、写锁争用，检查没有事件13和额外文件事务。原有200组pthread完成交错测试保留。

`test_verify_link.py` 的正确 fixture 已使用固定旧目标；共22个负例，包括全部12个旧符号与对应错误分支同时保持ELF/MAIN一致的负例，以及3个C绝对转发目标负例。全部必须拒绝。fixture仍是 synthetic stub 链接，绝不是实际全量链接。

验证记录见 `REVIEW2-VALIDATION.md`。没有连接USB、刷写、操作相机或运行QEMU；真实全量链接、硬件执行、native I/O时延和深层栈仍未验证。
