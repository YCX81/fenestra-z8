# armed-only recorder v3：入口、返回与拒绝证据

本版本针对 `ANALYZE1-RESULT.md` 的事件 0 单独留存盲点。仅本地源码、编译对象和测试；没有 USB、刷写、操作相机、QEMU 或 Unicorn。新版本尚未硬件验证，也不保证只凭卡文件能唯一定位挂点。

## 入口与不变项

共享 `live_w5_bringup_entry` 的 armed 路径顺序为：保存完整帧 → `fr_before` → 恢复入口 x0–x7 与 NZCV → 原 `live_w5_bringup_notify` → 事件 1 门控/探针 → 恢复完整帧 → 原 stock 指令/continuation。原 `live_encoder_preempt` / hand-back 实现逐字不改，STOP/TRIGGER/FINISH/EXIT 和超时不改。hand-back 调用时 SP 为入口 SP−784（未 armed 的旧 hand-back 为入口 SP−256，相差 528 字节）；此深度不含被调 C 函数内部栈。完整帧仍为 784 字节；整数、SIMD、NZCV、FPCR/FPSR 及入口 x0（含 service 返回值）恢复照旧。

未 arm 时仍用原汇编查找/身份/范围门控，只保存 x9–x12 与 NZCV 的 32 字节 scratch，不调用 C、不保存 SIMD、不取计时器、不写共享 RAM、不做 recorder I/O。完整有效链、arm=0 的六入口均为 **111 条指令 / 32 字节峰值栈**；无效链为 14–107 条。runtime extent 立即数随新布局变更，门控指令数不变。共享入口拒绝后照常执行旧 hand-back，其原整数帧/深层 C 栈不计入 recorder 新增成本。

只有有效 runtime、arm 非零、**完整保存 LR 精确等于 `0x40d07ba8`** 才进入入口/拒绝记录。client 不做门控，完整原始 X0 与实际 u32 client 都写入。事件 1 同样取消 client<2 限制；service 后续身份使用实际 client。未 armed、其他 LR、无效 runtime 不写文件。

## 固定扇区与单次策略

每个文件为 **3072 字节**，六个固定 512 字节扇区；无论事件顺序、缺失或争用，都不压缩、不移动后续记录。物理扇区不是时间顺序，应结合计数器和事务阶段。

|扇区/tx索引|事件|含义|
|---|---|---|
|0|0|worker 准备，排他创建并预写全部六扇区|
|1|15|REC 任务中，hand-back 调用前入口；不做 client、模式、ready、epoch、shutdown、generation 门控|
|2|1|hand-back 已返回，资格通过并发布 REC 身份|
|3|16|精确 REC LR 下的首次拒绝/epoch 变化标记；整个 boot 最多一次尝试|
|4|2|service-open 返回值；原栈/LR/client 身份核对不变|
|5|13|adapter 完成通知调用已返回；不代表录制成功|

事件 4、14、5 仍是 RAM 生命周期探针。事件 15/16 不调用 Live 快照、不跟随 `encoder.hw`；仅读 runtime、保存寄存器、模式及自身事务状态。事件 1/2/13 沿用原快照，其中 Live 部分仍会读取 `encoder.hw`，并非全部固定地址。

入口 `entry_claimed`、拒绝 `reject_claimed` 使用 CAS 且 boot 内不复位。入口失败、写锁争用或原生调用返回错误均不重试入口；返回后仍调用原 hand-back。已消耗入口后禁止重新 prepare，防止另一准备代次覆盖证据。拒绝记录独立于 ready/shutdown/arm-generation 门控，写入原准备 generation 对应的文件；epoch 变化只标记，**不阻止本次 REC 身份及后续新 OPEN 事务**。若没有有效准备 generation，或本runtime未完整成功准备该文件（排他 OPEN、WRITE 返回 0 且写足 3072 字节、CLOSE 返回 0 后才置 `file_owned=1`），则没有可用目标文件，记录错误 `0xf004` 于 RAM，不尝试打开编号 0 文件，也不覆盖排他创建失败时已存在的旧证据。准备失败后，入口、拒绝及后续 REC 事件均不执行文件 I/O；即使准备 WRITE 已写足而 CLOSE 返回失败，也不获准后续写卡。

|原因位|含义|
|---|---|
|0x01|当前 runtime epoch ≠ 准备 epoch，只有标记作用|
|0x02|shutdown|
|0x04|arm 与 generation 不同；检查期间取消 arm 也可能出现|
|0x08|ready=0|
|0x10|claimed 或入口已经消耗|
|0x20|照片模式，不符合 REC 关联|
|0x40|写锁占用/单次独占失败|
|0x80|claimed CAS 后复查失败|
|0x100|拒绝采样位置在 hand-back 返回后；这是位置标记，不是拒绝原因|

多个原因合并位图。epoch 单独变化也写事件 16，但继续事件 1。第一次拒绝若遇到锁争用或 I/O 失败，消耗该 boot 的拒绝机会，只留 RAM missed，不循环补写；因此“拒绝不静默”是单次尽力记录，不能保证介质留证。早期 epoch 标记也可能先消耗拒绝扇区，后续不同拒绝不会覆盖它。arm=0 的外层门控继续零 I/O，不能为了记录取消 arm 而增加未武装路径成本。

## 文件事务与干扰

文件名仍为 `A:\W5F00001.BIN`（token 1，以此类推）。新 token 为 1..99999；worker 在照片模式、LA_READY、无 writer/job、未 shutdown 时排他 OPEN `0x101`、WRITE 3072、CLOSE。已有文件不覆盖/删除，准备失败后只允许尚未消耗入口/会话时换更大 token 重试。

每笔后续事务均重新 **OPEN → SEEK EOF（必须3072）→ SEEK固定偏移 → WRITE512 → CLOSE**。不保留句柄、不追加、不重试。正常 REC 比旧版增加一笔入口事务（共四笔512写），若有拒绝标记则最多再增加一笔。即使 epoch 变化，也不复用旧句柄；卡更换、路径存在但属于其他介质的风险依然存在，长度检查不等于文件身份认证。

原生 I/O **无已证明的超时/持久化保证**。写调用返回错误时控制流继续 hand-back；如果调用本身永不返回，无法满足“继续 hand-back”。新增入口事务发生在 hand-back 之前，会改变 REC 时序、FS锁竞争和任务栈压力，可能掩盖、移动或新引入死机。不能把新增记录器的行为当作原故障的无扰动观测。

六项 RAM tx 各为六个 u32：event、phase、error、written、close_rc、serial。seq 现在是当前事务的**固定槽索引**，不再是已成功记录数。

|phase|含义|
|---|---|
|1/2|OPEN 前/返回|
|3/4|EOF SEEK 前/返回|
|5/6|offset SEEK 前/返回|
|11/12|WRITE 前/返回|
|13/14|CLOSE 前/返回|
|16/18|整个事务调用返回成功/失败|

文件记录携带构造时六槽 phase；入口/拒绝另带六槽完整结果，能够把已经返回的 I/O 错误转存到后续记录。当前事务结果当时尚不存在，末笔结果只在 RAM。phase16 不证明介质已持久化。灯在事务前亮、返回后灭，但与 stock 共用，不能独立定位。

## v3 格式与离线判定

魔数 `0x33524657`、版本 3、字127=前508字节 FNV-1a。decoder 同时检查魔数、版本、校验、固定扇区对应事件号。v2/2048字节记录不会被解释成 v3。

公共字段：字0..4 magic/version/固定槽/event/site；5 shutdown；6..7 ticks；8..9 prepare session；10 client；11 w0；12..17 原始X0/X19/LR；18..20 准备I/O结果；21 prepare epoch；22..25 kbps/size/arena；26 missed；27 seen；87 arm；117 mode；118 generation；119..124 六槽phase；125 原因/位置位；126 当前runtime epoch。

事件1/2/13 的28..116为原快照（87现为arm）；事件15/16的28..63为六槽完整tx。准备及轻量记录的 Live 字段**未采样**，decoder 不将重叠的tx字段误解释为Live状态。session为计数器样本，不保证跨boot唯一；状态不是跨字段原子快照。

|复位后卡上证据|能证明什么 / 仍不能排除什么|
|---|---|
|只有0，无15/16/1|仍不能区分未到hook、runtime/arm无效、入口I/O卡住或未持久化|
|有15，无1/16|已到hand-back调用前；可能入口事务尾部卡住、hand-back未返回、返回后的事件1快照/I/O卡住|
|有16，0x100未置位|记录了调用前的拒绝/epoch条件；不证明hand-back已调用或返回|
|有16，0x100置位|hand-back已返回；结合原因及六槽完整tx，识别ready失效、入口I/O返回错误等|
|有1|hand-back已返回；该笔WRITE/CLOSE尾部或后续stock仍可能出事|
|后续记录显示某槽phase18及error/close_rc|那一事务已经返回失败；不是“仍卡在该调用”|
|有2|service-open已返回；rc=0也不等于adapter/录制成功|
|有13|完成通知调用已返回；不证明MOVRS醒来或录像成功|
|后续记录携带某槽phase16|该槽调用已全部返回；非介质flush证明|

**单一卡文件不能严格完成五分类。** 尤其“hand-back内部卡住”和“hand-back返回后、记录器I/O卡住”可能产生同样的最后有效扇区；只要写证据也依赖会挂的I/O，就存在无法消除的最后写入歧义。本修改增加可证的入口、返回、拒绝与已返回I/O错误证据，不声称解决该不可观测边界。下一次若仍只有0，也不能宣称没到hook。

## 布局与状态工具

AArch64 layout：runtime=26512、recorder偏移=23184 (`0x5a90`)、recorder大小=3328、tx偏移=104；file_owned在60、entry_claimed/reject_claimed在96/100；runtime epoch偏移=1744 (`0x6d0`)、shutdown=1740 (`0x6cc`)。编译断言约束runtime总长与arm偏移；需全量重编 runtime 使用者。

`arm.py` 默认只读本地 layout.o，不导入 USB 模块。layout导出上述偏移及file_bytes=3072、transaction_slots=6；显式硬件状态读取分支会打印 **runtime_epoch、shutdown、prepared_epoch、epoch_changed**，读取六项tx，武装前检查entry_claimed。本轮仅使用离线模式及假读函数测试，没有执行其USB分支。

```sh
.recorder-venv/bin/python src-fix35/recorder/arm.py recorder-build/recorder-check/layout.o
.recorder-venv/bin/python src-fix35/recorder/decoder.py local-copy.BIN --json
```

## 本地验证与构建

生成全新overlay目录（不能已存在），然后测试；同步后的recorder-build可直接运行同样测试：

```sh
python3 src-fix35/recorder_patch.py --out recorder-output --check
.recorder-venv/bin/python src-fix35/recorder/verify_static.py recorder-output
.recorder-venv/bin/python src-fix35/recorder/test_gate.py recorder-output
.recorder-venv/bin/python src-fix35/recorder/test_frame.py recorder-output
.recorder-venv/bin/python src-fix35/recorder/test_io.py recorder-output
.recorder-venv/bin/python src-fix35/recorder/test_recorder.py recorder-output
.recorder-venv/bin/python src-fix35/recorder/test_verify_link.py recorder-output
.recorder-venv/bin/python src-fix35/recorder/test_forward_stack.py
```

门控测试使用限定A64指令解释器，armed保存壳测试用会破坏寄存器的C调用stub。host I/O测试替换native/RAM/写锁LL-SC，生命周期及一次性claim原子使用host pthread交错；均不是硬件执行。`test_forward_stack.py`仍针对现存旧版 `linked-recorder`，只算既有验证器回归，不是v3最终链接验证。

`verify_link.py`保持原样，不扩展CFG证明；负例夹具改为使用自身符号与探针位置，避免新入口长度改变使旧绝对偏移失效，原负例与证明范围保留。没有把测试夹具通过称为真实全量链接通过。

`relive_recorder.py`仍是诊断专用全量离线重编入口（SRC IMAGE MANIFEST OUT及`--relive`），需要对应bg34 SHA的manifest、所有者relive编译宏、clang/ld.lld/llvm-objcopy。禁止用旧build.py或混用旧layout/payload。保留core区 `[0x401fe000,0x401fff80)` 和USB尾部；最终布局/容量/verify_link需对新构建另行验证。本轮不产生可刷写的新固件，不以旧 `linked-recorder` 为v3验收依据。
