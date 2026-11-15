# 快门声：原厂拒绝条件与私有让路原型

以交付 v52 G `ed1e8496fccde443412e527a252de5029a55c749053b9b046da42bfc454aa613` 为只读原厂指令基线。没有改生产源、固件、共享 QEMU、USB、相机或物理卡。这里不是可刷包，也没有把 host/ARM 单元通过称为真实发声通过。

## 已证实的两层门槛

原来的被动 EV60 日志只记录进入 mute hook 之前的 stock mute，不能证明 Live 的 `mute_live` 设置返回了0。v52 G 在 `40f9cb64` 调用 Live gate；非零在 `cb68` 跳过 fire。这仍是当前真机必须区分的一个分支。

即使有效 mute=0，声音请求也会在更下层拒绝。原厂 `40e34d94` 调用 `40c80aac ->40c83908`，取得 driver `a02d4cb48`；`40e34eb8..ed8` 的 mask `0x31` 拒绝状态0、4、5。Live 录音是5。真实 ARM `sound-admission.json` 覆盖3种 tone×7种driver；`sound-open.json` 另确认服务 OPEN14 在录音5时也拒绝，不能只跳过上层判断。

这些是精确源码/原厂指令证据，并非已读到本次用户相机的唯一实际分支。

root随后提供的新G真机复制件已读取，见`card-v52-report.json`：LOG SHA72368336、PREV02aa0e41均无EV60；其header log_head/crumb_head仅3/3与4/4，now分别0.801秒与30.639秒，内容只含模式/UI事件。不是这些快照内把拍摄探针挤出ring；G源码只保留一代PREV，这两个低head快照没有9060～9063拍摄历史，不能由此判断prepare/fire没有执行，也不能区分当前硬件的两层mute门槛。仅读取工作目录复制件，没有操作卡。

## 普通一次快门的完整软件链

`prepare-fire.json` 实际执行正常 tone-set0、立即时间、不静音的 prepare/fire 两段：

1. `40f9cccc` 保存 prepare 数据并通知事件2；SoundTask `40f9c5cc` 选出 prepare1/fire2，`40f9caf4 ->40f9ce60` 发第一段。
2. `ce60` 先置 `a03bc7010=1`，调用 `40e312d8 ->40e3133c ->40e32e6c`，创建24B参数包装、40B声音参数、56B队列 envelope。顶层接受后设置 `a038fe9f1=1`。
3. 在 mic5 下，原厂 dispatcher `40e34b2c` 运行拒绝处理，调用 inner `04102004/04102005`，参数 `{tone,0}`。实际 `40f9d194` 完成 SoundTask 状态/事件。
4. dispatcher outer `04102000`，包装 status=1（start失败），走 `40e32ed8 ->40e3159c ->40e320e4/40e3212c`，SFX idle时清pending、处理延后的设置请求，再释放三块 heap。
5. `40f9cd54` 保存 stock mute并通知事件4；SoundTask通过 cb64 mute、inhibit、既有声音结束等待后，`40f9cc04 ->ce60` 对 tone2重复同链。

两段不是一个 fire beep；只接 cc04 会漏第一段。prepare 的定时/长曝光和循环 tone 分支未由普通一次测试覆盖。`ce60` 忽略构造函数的 bool返回，caller也忽略ce60返回；不能把它当“已发声/已结束”。当前 fixture 只把原厂配置、曝光比较、OS、日志、音长查表当输入/桩；producer、正常任务选择、构造、完整拒绝、原厂回调都执行。

## 为什么不能直接延后一个 queued 请求

`sound-cancel.json` 保留实际反例：自有代码暂存030 envelope后，若原厂031先到，它把SFX state置2；随后030虽然收到失败回调并释放，pending仍1/state2。因为从未启动DMA，没有下一次硬件完成来收尾。仅保存4个标量并假返回queued，或用原厂STOP盲撤销私有队列，均不完整。

对尚未转交 native queue 的请求，合法软件取消可以完整报告失败。私有 `held_sound.c` 已把这一最小协议做成实际AArch64原型：只接受 normal tone1/2、repeat1、精确原厂 envelope/callback，且原厂SFX state0、无foreign deferred callback。在原厂串行SFX消费线程内，CAS取得尚未转交的唯一持有权，按原厂失败顺序 inner2004/2005 `{tone,0}`、outer2000 status1完成held start；再以原始031/032 envelope的2001/2002完成软件取消，status0。这里inner参数第二字0不能单独叫作失败标志：accepted路径的2004也为0；真正区分是完整2004+2005拒绝序列、SFXstate0和outer start失败1。没有给不存在的硬件事务发送STOP，没有直接改原厂SFX/pending变量。

`held-sound.json` 的14项实际编译ARM测试：两tone×两cancel均清pending/soundtask/SFX、全部5块heap恰好释放；重复完成无效；empty/claimed/transferred、foreign owner、loop、不同callback/消息均不触碰分配和回调。**该helper尚未绑定生产hook**；它依赖原厂串行线程与严格未转交保证，不可由Live worker并发调用。generation/FIFO重用、enqueue发布与031/032顺序必须由完整adapter保障，当前不是已证明的条件。

## 原厂线程、队列与重投递顺序

`40e30c78` 注册任务 `a038fe980`、入口 callback `40e30d18`，`40e32e54` 把同一任务指针保存到 `a038fea70`。通用线程 `40c00b70` 一次 receive、一次 callback、返回后才再 receive；`30d18` 对030～033尾调 `34b2c`。这些反汇编保存在 `sound-thread.txt`。

`40c00bac ->40d2c128` 对消息统一写priority2；实际内核 `4017cc84` 等优先级尾插、`4017cf00` 头取。`sound-mailbox.json` 在已初始化且没有等待者的原厂mailbox上实际执行5条插入/取出，FIFO精确。TLS正常线程状态、create flags2/maxpriority3都是显式fixture；未声称已经测了并发sender调度。

因此安全方向是Live worker仅给**同一原厂队列**发稳定control envelope，由SFX原线程重投递；不能在Live worker CAS后直接重投，取消可能抢在post之前。`held_queue.c/h` 已把这一独立协议编成ARM并测试：cancel先于wake、cancel后新请求遇旧wake、generation匹配、一次queued标志、明确未queued的失败重试、not-ready保留、转交后只委托native取消。`held-queue.json`不声称已经绑定原生hook或完成真实麦克风/DAC。

`test_sound_accepted.py` 另执行原厂manager→dispatcher→正常accepted→032 hard STOP→完整回调：accepted只有inner2004、outer0且SFXstate1；STOP再发2005、outer2002并清pending/SFX/SoundTask。下层OPEN/配置/START/STOP是显式success shim，不是假装实际DMA通过。它只证明**已经交给native的请求必须走native取消**，不能再用私有队列失败completion回收。

尚未接合：精确photo请求来源与Live mic所有权predicate、50ms停麦ACK后的control通知、normal done对应自己的事务、固定wake envelope的实际queue lifetime，以及系统handoff/关闭/新拍摄重入。普通tone以外的loop/bulb语义未放行。调用顺序组件通过不能代替这些接合。

## 录音块可缩短，PCM不能直接 stop/start 丢掉

`capture-blocks.json` 运行实际G的109/106/709/111路径，50/100/250/500ms请求均准确传递到ADC descriptor及callback。500ms是当前Live请求值，不是该API固定协议。50ms请求为2400帧/9600B，原厂ring仍同一地址、count5，使用范围48000B。尚未证明真实DMA cadence、IRQ稳定、mic settle或10倍callback调度负载。

私有4源 `work/audio` 改为50ms×40个stage槽，仍384000B/2秒容量，backend struct744→1040B；若将来合入需更新runtime ABI。`pause`只停backend并等待108 ACK+idle+callback drain，保留核心active、descriptor、已有PCM和press mark。`resume`重开backend，在第一块前显式按绝对48k采样时标填静音；不压缩音频时间，不把意外overrun当合法静音。

`prototype-tests.json` 重新编译实际私有源ASan/UBSan，覆盖保留0.5秒预录、精确300ms静音、后续PCM和1.5秒绝对窗口；取消、partial start失败、停止ACK、in-flight callback drain、关闭期间及新GO之前的迟到回调、40槽溢出不覆盖。旧callback若在新GO之后才到仍无法单靠现有签名区分，需原厂队列/stop排序证据。不得将当前测试写成全部latecallback已解决。

`queue_prototype.c` 是另一个纯host有限两请求状态机，不是原厂adapter：pause确认后start、真实done后resume，handoff取消绝不重开麦克风，超时保留owner而不伪造成功。只允许普通1/2 tone。它的通过不消除上一节的native FIFO/线程/回调接合需求。

## 下一步与最小观察

继续在隔离环境把原厂dispatcher持有/重投递与031/032取消接合。必须明确“原队列尚未拥有/已重投递/已开始”三个状态，原厂声音任务之外只发控制通知，不能从UI/IRQ阻塞等Live worker。仅在真正停止mic且确认idle、无foreign owner后重投递原envelope；实际结束回调后才能恢复mic，并填实测缺口。

若整机模型不能覆盖真实声音，最小被动观察只需一张普通Live：有效mute返回（cb68的w8）、prepare/fire tone、下层driver读取（34eb4）、拒绝34ed4或进入34f08、inner2004/2005及其tone/result、native108ACK和恢复首块时间。不读scaler MMIO、不强开音频、不改用户静音。现有EV60没有覆盖有效mute返回与下层结果，因此仍不足区分本次硬件反馈。

空间上root当前primary仅64B；任何后续候选必须单独核算extra区并保留`401fd000..401fd218`，本原型没有提供可合入地址/镜像。

复测：`python3 run_prototypes.py`；ARM测试使用`feat-rec/.recorder-venv/bin/python`分别运行`test_sound_admission.py`、`test_sound_open.py`、`test_capture_blocks.py`、`test_sound_cancel.py`、`test_prepare_fire.py`、`test_held_sound.py`。所有输出仅在本目录。

新增复测入口：`test_sound_mailbox.py`、`test_held_queue.py`、`test_sound_accepted.py`。源/输出SHA清单在`sha256.json`，未列任何生产交付。
