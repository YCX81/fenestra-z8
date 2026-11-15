# v52 音频修复与快门诊断

2026-11-14。此目录只含离线源码、host 测试和 QEMU virt 原厂指令验证；没有操作 USB、相机或物理卡。完整固件链接/整机 QEMU 由 root 集成。本报告不把仿真协议通过等同于真机可听。

当前修复补丁 `audio.patch` SHA-256：`9f050b7dfa21b85ba65c768ef774a3a8117635b81b8bcd848acffc6a6cdfb8b1`。6 个文件的 old/new hash 在 `changed-files.json`。`audio-v1.patch` 仅为早期候选留档，不应拿它交付。所有原厂指令实验基于最终 v51d MAIN：`aa6e94e0042fb445c666de73c0f6b9e7e569010b7993a3ac3da0979b174a9433`。

## 用户观察与已确认问题

- 用户确认 v51 Live Off 时 9041/9042/9043 原厂回放均有声音；“无声”指按 REC 预览刚拍的 Live。不能据此认定真实新拍 MOV 已有正确音轨，需要检查实际文件。
- 用户确认 Live On 开始原厂视频均沙漏。代码中 worker 在音频停止前发布 W5 ACK，且音频 want 没有排除 hold。`/root/playback` 负责该 runtime 修复，本补丁提供可靠 stop 契约。
- 历史真机记录支持 Live 麦克风开启时快门声路径未到旧 mute hook；单独停麦克风后快门声恢复。当前没有新硬件观察，快门声尚未宣称修复。

## 修复内容与证据

### 原厂 payload 越界

`0x42040104` format 消息会在 payload `+0x20` 写结果；旧 `format[0x20]` 越界到 `channels[0]`。`0x42040109` ring 消息会在 `+0x30` 写结果；旧 0x30 字节 ring 越界到紧邻的 `stage` 指针最低字节。

`test_native_payloads.py` 执行最终 MAIN 的原始 dispatcher：旧 layout 的 channels canary `A5→00`、stage `4e008000→4e008001`，修复后都不变。见 `payload-old.json`、`payload-fixed.json`。修复扩展为 format 0x28、ring 0x38，保留原有效参数。

### 麦克风停止必须真正完成

旧 stop 最多等两秒就清 active/free，即使 stop 未消费、刚进入回调仍复制也一样；排队 start 尚未执行时，原厂 state 本来就是 idle，也会假完成。

新 stop 每轮只做有界非阻塞工作，保留资源直到本次 108 的回执、原厂 idle 和已进入回调数全部满足。没有回执不重用/重发仍可能在队列中的 message；超时记数并保留。发送失败可稍后重试。关闭回调准入与在途计数共用一个原子字，避免“检查完计数后又进入”窗口。partial start 失败同样等待清理，不能释放 queued payload 使用的 buffer。

`test_native_stop_callback.py` 执行原始 108：state4 时回调先看到4、函数随后发布1，证明不能只认回调；state1 的幂等停止也会回调但 result0。native running state5/DMA 完成只作静态追踪与 mock 测试，未假称真实 DMA 已测。

与 Live worker 契约：audio want 在 hold 时 false；active 且 cancelled 时继续 stop；W5 ACK 在音频 step 之后，需 active=0、descriptor=0、phase=IDLE。正常停完 cancelled 仍是1，不能裸用 cancelled 永久禁止下一次 start。

### REC 预览准入、完整音频和归还

原 PB 只尝试一次：麦克风尚占 driver 时直接丢弃声音。现在在 PB worker 的 stage7 最多等1.5秒，退出/关机可取消；不在 UI/原厂锁上下文等。失败只影响音频，不无限挂住视频。

原 PB 固定拷 2.4 秒，不能覆盖约3秒 Live。原厂 `40d24148` 实际申请 1,000,000 B，并把容量写到 descriptor `+8`。`test_native_pb_capacity.py` 执行原厂 allocator/init、service1a 和 native109，确认 960000 B/count1 正确传递并成功回执；仅锁/事件/排队作 RTOS shim。新 PB 先核对运行时容量与指针对齐，只用960000 B，载入当前 Live 可生成的完整0～4秒PCM，余下至少1秒清零。不会扩大原厂分配，或让4秒PCM越过环形区。更长的历史/外来音频仍最多取4秒，当前 writer 本身最多4秒。

停止按实际 PCM 帧数计时。native16 结果位在 payload[0]：成功即表示此 PB 已归还，不因后来 driver/service 被另一个用户占用而再次 STOP；失败只有原厂 service 仍处于2时最多重试3次。丢回执、异常状态或预算用完会保留 ownership 并报错，不复用未确认的 message。快速退出再 REC 会等旧停止完成。

`pb_audio_handoff()` 发布在 `FxRegistry.spare[0]`，只关闭准入并查询 ownership，无 I/O、等待或原厂调用。原子 gate 的 ownership 位在复制共享PCM前即占用，因此不能发生“stage0看起来空闲，其实正在写DMA buffer”。stage7尚未占用资源，关闭准入后可以 quiet；worker随后取消等待。高位 epoch 和 REC 键收到时的 ticket 阻止旧排队 REC 撤销较新的 native hold。真正新的 REC 仍须通过既有 native photo-owner/current 检查。

完整 integration 源检查只看到一个 publisher 和一个 Live reader 使用 spare[0]；它在 PbRuntime 清零/登记之后、worker 创建之前发布。创建 worker 失败时 gate 为空，查询仍安全。Live 侧仅在显式 W5 native handoff 期间请求 PB 停止，普通进入 view2 不应停 REC；此桥接由 root 集成，不能遗漏。

### 录音增益

v51 已将 writer 的均匀4倍增益应用到两个声道的全部样本；v52 没有降回1倍或添加 AGC。`gain-v52.json` 检查实际 callback。新增历史样本审计 `analyze_historical_pcm.py` / `historical-pcm.json`：三份 MOV 的 PCM 完全一致，实际上是同一份3.381秒真机记录，不能算三次独立观察。

其 RMS -47.87 dBFS，峰值2448/32768（7.47%，-22.53 dBFS），削波0。旧16385-frame callback 容量的半块4倍模型，143740个非零已放大样本全部可被4整除；错误地按16384帧建模有70个例外。只把原未放大的半块补成4倍，预测 RMS -44.81 dBFS、峰值仍2448、削波0。此预测不是 v52 实测录音，也不能保证较响环境不会饱和。

## 验证清单

- `test_lifecycle.c` 链接实际 audio core/backend，ASan/UBSan：queued start/idle假象、partial start失败、拒绝stop重试、丢ACK保留、真实pthread在途callback与停止竞态、迟到callback全部PASS。
- `test_pb_audio.py` 提取实际 PB 函数、仅替换地址与cache指令，ASan/UBSan：LVJA/LVJB、整段0～4秒逐样本一致、分块边界、零尾、canary、文件截断/读失败、容量不足、wait/cancel、连续REC、成功ACK后新owner、最多3次失败stop、lost ACK、handoff插入PCM复制与旧REC ticket失效全部PASS。
- `test_cfr_audio.c` 链接 timing 的实际 stream/capture/au_ring 和此 audio core：7种 epoch 相位×5个 press时点共35窗口，音频/视频同窗、时长误差≤一个48kHz样本、Apple所选帧包含press，ASan/UBSan PASS。timing 的最终3文件 hash 已与其 manifest核对。
- audio core/backend 和 PB runtime 均已独立 AArch64 strict-align、`-Werror` 编译。完整链接仍需 root 使用最新 header 重新生成 offsets。

Audio z8结构712→744 B，Live runtime 单独因此23184→23216 B；`new-offsets.json` 是只合音频时的值，叠加 CFR 后不能复用。PB 追加 diagnostics/gate/ticket，需重生成 PB offsets。不要为保持旧大小恢复越界。

## 快门：已有证据、剩余分叉与可执行诊断

原厂 prepare 生产者 `40f6cff0→40f9cccc` 投递 bit2；fire 生产者 `40f6e174→40f9cd54` 投递 bit4。SoundTask `40f9c5cc` prepare 路径可能在 `40f9c9f8` 等前一音效完成，早于旧 mute hook。

`test_native_shutter_gate.py` 在 QEMU virt 执行原始 fire 路径：state12时 driver1/5均到 mute hook；mute=1或inhibit=1仍到hook，但不进入音效；state11/0在hook前被过滤。DAC入口/logger/RTOS event-clear 为stub，事件生产与prepare等待尚未模拟。该证据排除了“fire路径只因driver5就跳过mute hook”，没有证明按快门时实际是哪条门槛。

没有跳过原厂判定，也没有在按快门回调里阻塞停麦克风。新增独立 `shutter.patch` SHA `d16ff1150d2e32e99ead79318f9136dd2b549ded4ee310dd74996e7435d32fbf` 是**被动诊断**：full press、prepare投递、fire投递、旧mute点分别把驱动/服务/sound状态、mute、inhibit写入既有RAM crumbs，以EV60随原有worker保存 `LIVEDATA/LIVELOG.BIN`，不需要USB。`read_shutter_log.py <日志副本>` 可离线解析。记录可能被后续事件覆盖；缺少单条记录不能独立证明分支从未发生。

建议现场流程：上电超过20秒，在取景保持Live开、声音On，拍一张并等Live文件保存完成，再在取景等至少5秒；然后同样做声音Off/Live Off对照。复制卡上 `LIVEDATA/LIVELOG.BIN` 与 `LIVEPREV.BIN`、对应MOV/JPG，先用 `read_shutter_log.py` 看同一时段四点与音频状态。不要求USB；此任务没有执行现场动作。日志丢失/写失败仍可能，须同时保留用户听感与文件。

## 额外代码段集成

诊断C和两个wrapper在 `.live_extra`：实测 object sections 712 B，原主段只增加24 B；见 `shutter-object-sizes.json`。32组真实wrapper指令 Unicorn 测试验证 x0～x30、NZCV、原参数和返回地址完整保留，`shutter-wrappers.json`。它不是完整固件运行结果。

linker主段结尾先设 `__live_primary_end=.; ASSERT(__live_primary_end<=0x401fd000, ...)`，再添加：

```ld
.live_extra 0x401fd218 : { KEEP(*(.live_extra)) }
ASSERT(. <= 0x401fe000, "Live extra cave exhausted")
```

把 root 的 PB handoff helper 也放该段即可共享空间。不要拼带中间洞的整块 objcopy binary：`401fd000..401fd218` 已有尺寸补丁。`split_live_payload.py --elf ... --out-dir ...` 严格提取 primary.bin/extra.bin，拒绝洞内section、writable/BSS与重叠；可选 `--image ... --expected-sha256 ... --output ...` 在已拼对应primary的 MAIN 上添加 extra 和两处严格原字节守卫 BL，验证全量还原与尺寸补丁区不变。`test_split_live_payload.py` 六项反例/还原测试PASS。

## v52c 整合音频覆盖

被动快门诊断已有真实整机写卡/取回证据：生产C、Cdiag和私有协议C三轮的 `LIVELOG.BIN` 均包含全部4个EV60点，录音软件状态driver5/audio_active1，prepare/fire由60/60变为1/2、prepared由0变1，且到达mute_gate。完整状态、时刻、manifest与缺失覆盖说明见 `SHUTTER-QEMU-RESULT.md` / `shutter-card-evidence/report.json`。这是诊断落卡链路验证，**不是快门声修复或真机可听验收**。

最终整合 `pb-fix37/runtime.c` SHA-256 `ba1cfbe8d6ff4fc4b46c5aea8a7ea8c37e035aa4286ef5db8bb40a856524994c` 与本目录最终源、远端 `integration-c` 一致；在整合目录重新执行 `test_pb_audio.py`，完整 ASan/UBSan 回归通过，记录为 `pb-audio-integration-review.json`。

模型核对结果保存在 `qemu-audio-model-audit.json`，含源文件 hash 与逐行摘录。当前 `merged/work` QEMU 仅保留 Serial5 配置，没有 TX DMA 消耗/完成 IRQ；其 FIFO 空返回明确限于没有 producer 的模型。现成 `rdw` 有读取实际 RAM 的 TX drain，但缺少当前拍摄所需的 `wave5-upstream-model`、`z8-heif-secondary-stop-model` 等属性，不能直接替换同一 core driver。

为补充整合服务路径测试，独立构建了 **仅供仿真、禁止交付** 的 `PB_AUDIO_TEST` 镜像：源与 v52c 完全相同，仅 PB 编译器追加 `-DPB_AUDIO_TEST`，MAIN SHA-256 `542c255303f44151552e1eb3706c9d152676b0bd4cc05258bc29ca7513e07d40`，远端目录 `/path/to/user/fxfix/v52-audio/protocol-test`。该宏把缺少音频的样本转为 **零 source frames**，仍调用原厂 open/buffer/start/stop；即便通过，也只证明该服务协议与 Live 恢复，不证明 3 秒扬声器 DMA、采样时序或真机可听。驱动只增加被动 callback 观察和原厂状态读数；完整记录由 `analyze_protocol.py` 解析。

该次 `v52c-audio-protocol3` 已结束。分项结果 `protocol-v52c.json`：**service_protocol_passed=true，live_recovered=false，整体 passed=false**。实际原厂 callback 顺序为 `43180014→4318001a→43180015→43180016`，最终 PB `audio_plays=1/errors=0/stage=0/gate=0`、原厂 `driver=1/service=0`；REC 45 帧完成、错误为0。取景恢复后 IRQ `1536→2034`，编码计数仍547，因而原 core driver 的恢复断言失败。生产 v52c 的无声 lane 也停留在相同的编码释放问题；不能拿本次音频服务成功覆盖整体失败。原始 checkpoints、observer、result、私有构建出处均保存在 `protocol-run/`。

## 独立核对 HEVC 关闭与 arena guard

在 root 的整机诊断发现 `inst[3]==3` 拒绝真实 HEVC 实例后，独立反汇编确认这是 codec 类型，不能一概硬编码 AVC：

- worker `40c3b2ec..308` 把 Nikon codec0/1 转为 open 参数 `+0xc` 的0/12；参数别名由 `40c3a6c0..6d0` 建立。
- `40c45304→40c40bd0` 按产品路由到 Wave5 `40c57ff4`；`40c58060..8c` 把参数0映射3、参数12映射1，并实际写入 `instance+0xc`。
- STOP `40c39194` 只设 stream EOS flag。worker `40c3b9d0..ba1c` 先设 EOS_DONE，下一触发 `sourceEnd(+0x44)=1`；Wave5 `40c59a88..aac` 写源索引 `0x144=-1`，无 AVC 限制。`40c59be4..bfc` 明确让 mode1/3进入同一分支。
- FINISH 经 worker event400、`40c35020→40c453ec→40c42304→40c5a43c` 发 command0x20；`40c541f4..208` 将实际 `instance+0xc` 放到寄存器0x110高16位，HEVC保留1、AVC保留3。

`test_native_w5_codec.py` 用原始 ARM 指令在 Unicorn 执行映射、handle验证、EOS源索引及命令写入，全部通过；VDI写函数和pool lookup为明确的测试边界。六段原始代码与最终 v52c MAIN4790c3逐字节相同，记录在 `native-w5-codec.json` / `native-w5-v52c-identity.json`。这些测试未证明完整 worker/EOS/硬件释放；整体修复和整机回归由 playback/root 负责，本 Agent 未改共享 encoder 源。

另发现新 owner guard 的基址误用：真正 VDI struct base 是 `0xa02d3d928`；它的 `+0x58=0xa02d3d980` 是 MMIO 基址字段。再对该字段地址加0x58读到的是 struct+0xb0，即第二条 DMA 记录的物理地址。原 BRINGUP arena 位于 **`0xa02d3d950` (struct+0x28)**，大小位于 `0xa02d3d948` (struct+0x20)：`40c3fae8` 保存原始输入到+3f08/+3f10，`40c3e9e8..ea08` 原样复制到+28/+20。common buffer由 `40c3ee34` 设为2MiB，首记录使用它，`40c3ea34..4c` 将后续分配起点设为arena+2MiB，因此错读第二记录才出现+2MiB。已将正确字段与指令位置交 playback；不要通过接受偏移值来放宽所有权判断。证据 `w5-arena-disassembly.txt`。 `test_native_w5_arena.py` 又以两个不同 arena/size 执行上述原始保存、复制、common记录及分配参数指令，全部通过，见 `native-w5-arena.json`；未模拟 allocator 生命周期或硬件释放。

新增符号绑定与hook地址见 `shutter-sites.json`。原厂目标仍为40f9cccc/40f9cd54；两处BL原字节37bf0094/f8ba0094。root 应将额外段和两处hook一起加入最终 manifest、重新链接并检查整机日志，不能只拷诊断源码而漏拼section。
