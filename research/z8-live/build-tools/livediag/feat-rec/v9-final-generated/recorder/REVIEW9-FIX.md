# REVIEW9 三项修订与验证

本次仅修改 `src-fix35/recorder_patch.py`、`src-fix35/recorder/`，生成文件与本地验证产物放在 `recorder-build/`。没有 USB、刷写、相机操作、QEMU。对象执行采用 Unicorn，替代原生服务、时钟和异步状态；不是固件启动或相机观测。

## 修改

1. **竞争关闭超时**：所有竞争路径（含原 worker close 和私有 preempt）在原 4 秒条件成立后才查询 RAM。只有 armed、已领取会话且锁对象等于该会话持有的对象时有宽限。六个持锁 D 分别记录起点和实际完成时长；宽限为已完成时长＋在途时长（单帧最多 1.25 秒）＋0.1 秒。没有 D 则为零。竞争者保留已观察到的最大值，避免 owner 撤销时突然缩短截止。完成 D 的实测迟调度／中止时间也计入。没有把 busy、输出、worker、原生事件或 VDI 的超时延长。

   D 自身新增 1.25 秒截止检查；DELAY 持续返回时会中止过长帧，置 lamp_failed 并释放灯。若 DELAY 本身不返回，竞争者只给有限在途信用，不会无限跟随当前时间。所有墙钟上限仍以竞争者能获得调度为前提；本地 1 ms 输入步长保留原 `>` 超时语义。

   RAM 位于已分配 io 第二扇区的 union；准备期间仍是原文件缓冲，REC 后不持久化这些字段。runtime=26512、fr_state大小、arm/skip_nr/NR ABI、门控全部保持。

2. **调用链归属**：汇编将固定会话指针和入口保存帧显式传给 notify，逐层传到 checkpoint。校验 `s==fr_context()`、claimed、I已尝试、未R/DONE、当前 SP 位于入口 SP 下方 784～4096 字节、保存帧指针恰为入口 SP−256、保存的完整 LR 为 `0x40d07ba8`。先检查指针／SP再读LR；任何拒绝都发生在 lamp_take 之前。栈更深时保守丢码，未宣称实际栈余量充足。

3. **VDI 判定**：D18 移到原 VDI 循环成功退出之后，或错误9已经选定的分支内；两条分支均只发送一次。EXIT之后的原 RAM记录及计时位置保留，D18不再位于计时起点之前。D18仍证明EXIT已返回，但本身不区分VDI成功或错误9，也不证明close成功。

未修改 NR 私有 EXIT、原生函数或 CFG 证明器。未 armed 时原失败条件、阈值和 DELAY次数保持，超时分支增加一次返回零的RAM查询；没有声称整个close对象字节完全不变。三个入口的未 armed 门控字节仍与v4一致。

## 全部 D 与既有预算、锁的核对

| D | 相对操作位置 | 预算核对 | 发码时已确认的锁 |
|---|---|---|---|
| 1 | 参数检查后、发布release_req之前 | busy计时仍在release_req之后；D不在4秒等待内 | 无close_lock；不跨FS/Wave5服务锁 |
| 2 | busy等待通过后、取close_lock前 | 取锁计时仍紧接CAS循环前；唯一预算变化是目标持锁D信用 | 同上 |
| 10 | 前置检查／可选SUBMIT返回后，STOP前 | 不在任何已有本地计时循环内；SUBMIT已返回 | close_lock |
| 11 | STOP返回、INFO基准采样后，首次TRIGGER前 | 原生事件参数1000不变；输出500ms计时仍在TRIGGER返回后 | close_lock；STOP服务锁已随返回释放 |
| 13 | 输出变化已经观察到，drain TRIGGER前 | 原生事件参数1000不变；worker状态5的1秒计时仍在drain返回后 | close_lock |
| 15 | worker状态5已观察到，FINISH前 | FINISH内部发0x400／等0x800、参数100均不变，内部无D | close_lock |
| 17 | FINISH返回及状态检查通过，EXIT前 | EXIT内部发4／等8、参数1000不变，内部无D | close_lock；FINISH服务锁已释放 |
| 18 | EXIT返回且VDI循环完成／错误9判定后 | 与无D路径同一1秒预算和判定；发码不能把已选错误改为成功 | close_lock；EXIT服务锁已释放 |

preempt最后仍在close完成后写preempt_us，后续reopen的3秒冷却长度不变；完成时间整体后移。D11/13/15/17延迟下一条命令提交，但没有插入其原生事件等待内部，也没有在启动后重设计时。整体同步诊断仍扰动调度和总耗时，不据此把v5重启归因于某个超时。

FS/Wave5锁依据：D仅在原生调用返回后／下一次调用前；原生服务二进制不变，沿用REVIEW9已核对的释放路径。checkpoint链只访问RAM、计数器及GPIO/DELAY，无FS操作或Wave5服务调用，也不取得准备锁。不把这些静态事实扩称为对任意隐藏锁或硬件争用的证明。

## 对象反例

实际执行最终ELF中的原close及私有preempt，两条竞争路径均得到：

| 输入 | 返回时间 | close_failed / fin_rc高字节 |
|---|---:|---|
| 持锁者0.2秒释放 | 0.200秒 | 0 / 0 |
| 六帧各1秒，持锁者6.2秒释放 | 6.200秒 | 0 / 0 |
| 六帧已发完，之后真卡死 | 10.101秒 | 1 / 11 |
| 首帧内真卡死，无后续完成记录 | 5.351秒 | 1 / 11 |
| 无D且不释放 | 4.001秒 | 1 / 11 |
| 未armed，6.2秒才释放 | 4.001秒 | 1 / 11，与原路径一致 |
| 其他锁对象，6.2秒才释放 | 4.001秒 | 1 / 11 |

`fr_before` 对 MOVS LR=`0x40cf9858` 返回0；仍有活动REC会话时，外来checkpoint不发D17，内存写钩子确认不写灯所有权。另测不同SP、错误会话指针／保存帧／64位LR、未armed、DONE、RETURNED及无I；真实REC链可发D17。

计时不是只用模型公式：真实checkpoint调用的emit边界延迟1.37秒时，RAM记录1.37秒，grace返回1.47秒；真实C波形还测试100ms调度步长下D截止、中止与实际时长累计。

VDI在EXIT后0、0.2、1、1.5秒清零，分别对比无D原close与带D的私有close（每帧0或1秒）。前3种成功；1.5秒输入都在1.001秒选定错误9。D18耗时1秒只让函数晚1秒返回，不改变选定结果。所有八个D的顺序也在完整私有close对象路径上检查。

## 回归与产物

最终产物：`recorder-build/linked-lamp-v6-review9-final/`。
MAIN SHA256：`62fdd6047d61725fb73f570629ac61e58c1eb539821fea06f3707fc095986ff3`。

运行命令：

```sh
.recorder-venv/bin/python src-fix35/recorder/run_checks.py recorder-build recorder-build/linked-lamp-v6-review9-final
```

完整结果见 `recorder-build/recorder-check/lamp-v6-tests/results.json`；对象反例详表见最终链接目录的 `review9-tests.json`。首次回归的test_nr曾因新增两条参数指令改变门控拒绝分支位移而失败；已复用现有指令位置传参，保留原门控逐字比较断言，不放松测试。

最终13项检查中12项PASS：verify_static、test_gate、test_frame、test_io、test_recorder、test_v6、test_verify_link、test_video_decoder、test_forward_stack、verify_lamp_artifact、test_nr、test_review9（37个对象反例）。唯一FAIL为完整verify_link的六条旧forwarding证明不完整，统一命令退出1；没有把它算成通过。完整verify_link的旧forwarding CFG失败不得称PASS；本次没有扩大证明器范围。产物仍为 `flashable=false`，没有硬件验证。
