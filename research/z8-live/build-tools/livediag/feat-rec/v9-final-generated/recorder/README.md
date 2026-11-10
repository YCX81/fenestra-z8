# v9：可缓存控制结构修复候选

基于 v8b 当前源码；实验室 MAIN `8866ea3b` 为用户提供的标识，本次未取得对应二进制核验。协议继续使用 `--protocol v8`（I/P 位宽、CRC、原阶段号不变）；新增15、17、49、54，20/22增加返回detail=1。旧v8b不会产生这些新值。

CPU控制结构使用0x40d2c408普通堆，DMA arena/region/slot仍由pool分配。初始化发布前失败释放两种分配；发布后（含部分open失败）仍保留到本次启动结束，release/preempt只关硬件实例，不释放 backing memory。详见 `../../runs/v9/REPORT.md` 与对象/分配回归。

# lamp-v8：RAM 进度与独立 worker 报告

基于修改前的 v7 `src-fix35/` 和 `recorder-build/`；备份在 `runs/lamp-v8-baseline/`。用户给出的实验室 MAIN 为 `f7f9132d`。本地 v7 linked MAIN 为 `0e0e0b8c…`；扫描本地 12 份 linked MAIN 未找到该实验室前缀，清单见备份目录 `main-inventory.json`。因此这是**指定 v7 源码的后继版本，尚未证明与实验室 MAIN 二进制一致**。

输入真机证据为 `runs/a4991/report.txt` 与 `runs/v7-4994/report.txt`：都只解出 I/D1；v7 D1 busy=0、alive=1，约27秒后黑屏重启。缺 D2 不足以证明任务卡死，也可能是发码不返回、原厂灯覆盖或漏解。

本次只修改文件、离线编译/链接、执行 host 与 Unicorn 测试。禁止且没有使用 USB、刷写、操作相机或 QEMU。产物 `flashable=false`，不是硬件验收。

## 数据与执行边界

- I 保持原有 50ms 半位、1.5秒名义帧及原 hook 保存/恢复。I emitter 返回并完成 GPIO 清理、释放所有权后，release 发布 `diag.i_done`。它表示 I 尝试已结束，不保证视频中 I 完整可见；失败仍记 `lamp_failed`。I 若不返回，worker 不接管。
- I 后的 D/R/S/M 不再发灯、不新增 DELAY、不做文件 I/O。原路径中的 DELAY 原样保留，每次只包裹 RAM 记录；不是删除关闭路径本来需要的等待。
- MOVRS 私有路径保留原有 `w5_diag` 调用，并记录调用前/返回后。这些旧调用仍读取原状态；v8 新增记录器不做硬件快照。没有把“返回后检查点”挪到调用前。
- `diag.packed` 是单个32位 release/acquire 快照：阶段6位、附加值3位。MOVRS 单写者；worker 只读，不等待 seqlock，不因生产者停在发布中间而阻塞。读到的是最后完整发布值。
- 阶段号不回退；循环在同一阶段反复发布附加值。RAM 另存 `updates`、`busy_iterations`、`delay_returns`、`timestamp_lo`（CNTPCT tick 低32位）、busy、alive、最近一次 DELAY 返回值、完整 S/M/R 原值。RAM 中这些辅助字段不是联合原子快照，光学帧只采样 `packed`。
- `diag.alive` 是最近一次已有 alive 检查的结果，循环记录不会额外调用 alive。最近 DELAY rc 在下一次调用前保留；0在首次 DELAY 之前也可能仅是初始化值，需结合 `delay_returns`。
- 会话、当前 context、claimed、入口SP/完整LR、未返回/未DONE 检查保留。R/S/M 仍用原 `matches` 检查；R/DONE 终止后续 hook 归属，**录像模式中不终止本次 worker 报告**。没有重新武装或复用后续 REC 会话。
- 未 armed 的汇编门控、hook 重放/保存恢复与保存的 v8b 一致；v9 仅共享 codec 的初始化分配改变（原子字段移到缓存堆），close/preempt/frame 对象代码不变；worker 复用已有 arm 判断，不增加第二次未 armed 状态读取。arm=0 时不调用报告器。没有新任务、IRQ或计时器。
- ABI 不变：runtime 26512字节，recorder 偏移0x5a90，RAM 扩展位于原 io union 的空闲区域。WFL4/version4、准备 word86=6、layout LP06 沿用旧值，不能用它们判定 v8；以 MAIN SHA、manifest 和解码协议为准。

## worker 与灯所有权

`runtime.c` 的无限循环只把 `photo && !shutdown` 传给 stream/audio 操作，`photo=0` 不退出、不跳过循环末尾 DELAY(5)。报告放在原有 arm 检查处、photo 准备条件之外；`fr_worker_tick` 要求同一 context、claimed、i_done，且 `0xa03bd062c != 0`（与 worker 的 `photo` 判定相反）。照片模式直接返回，不发 P、不调用报告 DELAY。P 帧每个边沿及每次 DELAY 前也复查模式；途中切回照片则中止、灭灯并释放所有权，不再等待整帧。已进入的原生 DELAY 无法抢占，需其返回后才能中止。按帧起点间隔约2.5秒发送，不补发积压帧，心跳序号每次发送尝试递增并模8回绕。I 结束后先等2.5秒，有助于避开首次约1.3秒原厂长亮，但不能保证避开。

记录器内 I 与 worker 使用原非等待原子灯所有权。I 结束后只有 worker 调用发码器；R/S/M 即使遇到灯忙也只写 RAM。worker 每帧名义1.25秒，硬截止1.6秒，DELAY错误立即退出并灭灯释放。硬截止仍需要调度机会，不能中断一个永不返回的原生 DELAY。帧失败计 `report_failed`，后续轮次仍可重试。

**独占只覆盖记录器，不能约束原厂 GPIO 写入。** 光学内容可能被原厂约1.3秒连续亮覆盖；CRC失败/漏帧必须保留，不能修位或补造阶段。

### 安全性证据与限制

1. 源码证据：worker 是任务入口，已有每轮 DELAY(5)；原 v7 worker 的 prepare 已使用同一 GPIO。v34/v35 的 `lamp_set/lamp_wait/lamp_code` 也用相同 GPIO/DELAY，但其 `bringup_notify` 调用历史本身不是“worker REC后安全”的证明。
2. MAIN 反汇编：`0x40c15c3c` 的0x44字节实现是无 BL/BLR 的叶函数；GPIO/DELAY/唤醒回调与既有参考镜像逐字一致。发码前沿用 `sleepable()` 检查 IRQ mask、TLS任务状态和调度条件，DELAY非零立即停止。详见 linked 目录 `lamp-audit.json` / `lamp-disassembly.txt`。
3. 未验证：原厂是否在 REC 后挂起/饿死该 worker、是否存在隐藏锁、GPIO可见性及真实时钟/调度延迟。报告位置之前仍有 UI、stream、audio、flag/log/ch2/view 调用，之后也有原有几何查询；任何一个阻塞都会阻止下一帧。帧内的 DELAY 本身也可能不返回。
4. worker 发一帧会占用约1.25秒墙钟，期间通过 DELAY让出CPU，但会延后这个 worker 自己的其它工作。这与 v7 在 MOVRS 内同步发码的扰动不同，仍不能称无扰动。

所以只能核实“源码中 photo=0 继续循环，调用上下文具有任务级检查”；不能离线保证“REC后 worker 一定运行、GPIO/DELAY在所有隐藏状态下绝对安全”。

## P 编码与解码

P 为新短同步阶段 `101 | progress[6] | detail[3] | heartbeat[3] | CRC4[4]`，总19位；I/R/S/M旧帧的头为2位，D旧帧头为100。P 同步灭100ms、亮125ms、灭75ms；Manchester 半位25ms，10=0、01=1。CRC x^4+x+1，初值0、MSB先行，无附加零位。名义时长 `(12+2*19)*25ms=1250ms`。heartbeat 0..7 回绕，不能把隔8帧的相同值当成停止。

默认 `--protocol v8`：接受 P，拒绝旧 D。历史 v6/v7 视频必须显式指定版本，保留原 D 解释；例如 v7 的 D1 detail=2 表示 busy=0/alive=1，不能当NR。阶段0/63为保留值；只依据独立位判读后CRC接受，不用CRC挑选相位。

### 阶段表

短瞬态可能在两帧间直接越过。P 是最新状态快照，不是逐事件历史。

|阶段|含义 / detail|
|---|---|
|1|I emitter尝试结束，已交接；可能尚未进入有效 encoder 路径|
|2 / 3|原D1处 w5_diag 调用前 / 返回后|
|4 / 5|alive检查前 / 返回后；5 detail bit0=busy、bit1=alive（独立采样）|
|6 / 7|release_req=1 写入前 / 后|
|8|进入busy条件判断前|
|9|每次busy迭代：bit0=busy、bit1=DELAY已返回、bit2=DELAY返回非零。before=1；after=2/3/6/7|
|10 / 11|busy循环退出 / 最终仍busy，走超时失败分支（detail=1）|
|12 / 13 / 14|原D2位置、其w5_diag返回、alive复查通过即将close|
|15 / 17|close_lock CAS前 / CAS成功（owner登记前）|
|16 / 18 / 19|close_lock等待DELAY前后 / 竞争超时 / 已取得lock进入close检查|
|20|SUBMIT：detail=0前、1返回|
|21|w5_diag(10)前|
|22|w5_diag(10)返回，STOP：detail=0前、1返回|
|23|w5_diag(11)前|
|24|w5_diag(11)返回，首次TRIGGER前|
|25|首次TRIGGER返回，输出检查|
|26|EOS输出等待DELAY|
|27|w5_diag(12)前|
|28|w5_diag(12)返回|
|29|w5_diag(13)前|
|30|w5_diag(13)返回，drain TRIGGER前|
|31|drain TRIGGER返回，状态检查|
|32|drain等待DELAY|
|33|w5_diag(14)前|
|34|w5_diag(14)返回|
|35|w5_diag(15)前|
|36|w5_diag(15)返回，FINISH前|
|37|w5_diag(16)前|
|38|w5_diag(16)返回|
|39|w5_diag(17)前|
|40|w5_diag(17)返回，EXIT前|
|41|EXIT返回|
|42|w5_diag(18)前|
|43|w5_diag(18)返回|
|44|VDI等待DELAY|
|45|VDI等待结束|
|46|w5_diag(19)前|
|47|w5_diag(19)返回|
|48|close_lock及owner清理后|
|49|w5_close_lamp返回|
|50|w5_diag(30)前|
|51|w5_diag(30)返回|
|52|release_req=0写入前|
|53|release_req=0写入后|
|54|preempt结束，即将返回notify|
|58|!alive早返回|
|59|notify返回前|
|60|R位置，notify已返回|
|61|S位置|
|62|M位置|

9 的 detail 保持原义。16/26/32/44 的循环 detail：0=DELAY前，2=返回0，6=返回非零；26/32 detail=4表示超时。18 detail=1表示close_lock超时；45 detail=0正常、1=VDI超时。60 detail=NR掩码；61/62 detail=原值低3位，完整值保留RAM。

阻塞点逐项核对：alive/close入口19、SUBMIT 20、STOP 22、首次TRIGGER 24、输出DELAY 26、drain TRIGGER 30、drain DELAY 32、FINISH 36、EXIT 40、VDI DELAY 44、close_lock DELAY 16、busy DELAY 9；各w5_diag调用也有独立阶段。相同循环的DELAY前后仍用同一阶段、不同detail，保持循环阶段不回退；不同阻塞调用不再共用阶段。首次/drain TRIGGER返回分别发布25/31，EXIT返回发布41。

本次重新分配v8关闭阶段20..48；帧位宽与CRC不变。修订前v8录像须使用其原版解码器/阶段表，单凭帧本身无法区分修订前后版本。

## 判定表

|观察|支持什么 / 仍不能证明什么|
|---|---|
|P阶段固定、心跳连续递增|worker的报告器仍获执行；MOVRS没有发布更晚阶段。范围缩到该检查点后至下一发布前，不能精确到单条指令|
|P=2长期固定|D1处w5_diag未发布返回证据；包括其参数求值、内部读数或返回后的发布路径|
|P=3/4/5/6/7固定|分别检查alive检查前、检查本身、release_req写入前后的下一小段；不能从采样间隔重构每条指令|
|P=9/detail=1固定而心跳增|反复采到DELAY前；可能DELAY不返回，也可能每轮都在同一相位采样。RAM计数不在光学帧内，不能单凭固定detail断言迭代停止|
|P=9/detail含bit1|至少一次DELAY返回已发布；bit2指示返回非零，不是调度健康证明|
|P=12..48|已有后续关闭路径证据；D2曾经缺码不再等于没有到达|
|P=41..47|EXIT已返回；不代表资源交接安全|
|P=60/61/62|原R/S/M路径到达，NR或返回值按上述字段解释；不证明录像成功|
|P停止或始终没有P|照片模式主动停报/worker被挂起/自身调用阻塞/发码DELAY不返回/系统调度停止/灯覆盖/录像漏解都兼容。**不能单独推出全系统调度停止**|
|只有I，无P|确认入口发码；不能区分I清理返回卡住、worker未到报告点、worker被挂起与光学漏帧|
|原厂连续长亮或CRC不通过|不是有效P，不能补读被覆盖的阶段；后续有效P可恢复观察|

单调阶段的错误/早退分支会跳到50/58/59/60；“阶段数较大”不等于所有较小阶段都执行过。固定detail也不等于RAM计数不变。

## 截止与本地验证

原 busy 截止仍是4秒；v7心跳耗时补偿已删除，因为hook不再发D。共享close_lock依旧先CAS、失败再检查无符号 `elapsed > budget`；预算仍是4秒，加armed且claimed时固定7.6秒。后者作为原安全截止保留，**不是仍有7.6秒持锁发灯**；时间槽不再记录D发码，且不用于累计credit。硬截止均不保证在任务不被调度时返回。

```sh
.recorder-venv/bin/python src-fix35/recorder_patch.py --out recorder-build/v8-new --check
ZIG_GLOBAL_CACHE_DIR=/tmp/feat-rec-zig-cache .recorder-venv/bin/python src-fix35/recorder/relive_recorder.py \
 recorder-build bg34.main bg34.changed-ranges.json recorder-build/linked-lamp-v8-new \
 --defines-json recorder-build/compile-defines-v4.json --zig .recorder-venv/lib/python3.14/site-packages/ziglang/zig
.recorder-venv/bin/python src-fix35/recorder/run_checks.py recorder-build recorder-build/linked-lamp-v8-review14-final
.recorder-venv/bin/python src-fix35/recorder/video_decoder.py INPUT.csv --sequence --protocol v8
```

统一套件包含门控、保存恢复/重放、生产C事务/会话/波形、所有P阶段值/心跳组合与单bit错误、120fps抖动、1.3秒长亮覆盖后恢复、实际链接AArch64的busy/close/身份/硬截止/独立报告、NR及完整verify_link。旧test_v7作为历史文件保留，其“同步D”要求由test_v8替代；其余相关回归已更新为RAM观察。v9 新增真实链接初始化、失败释放、地址对齐和独占目标地址回归。Unicorn原生服务/时钟/调度是桩，不是相机运行证明。

最终结果以 `recorder-build/recorder-check/lamp-v8-tests/results.json` 为准。完整 `verify_link.py` 原样运行，旧forwarding CFG不完整仍应明确报FAIL，不能称全部测试通过。本次修订源码diff见 `runs/review14-before/changes.diff`。
