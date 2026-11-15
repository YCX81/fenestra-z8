# v53 音频启动候选

候选 `capture-work`，基线 `work`（G capture + 已冻结 v53C PB speaker），补丁 `capture-startup.patch`，精确 6 文件 old/new hashes 在 `capture-changes.json`。只修改私有源，未刷机。真实录音结论见 `real-card/RESULT.md`。

## 修复的代码路径

1. 同一个 Live worker 在可能较慢的视频 step 前先 start/poll 音频，step 后再次 poll 并处理 mark/freeze。遇到尚未消费的 C0 soft-cancel epoch，前一次调用跳过，让 stream 沿原流程先确认；否则会在拍照时错误停麦。等待音频不会跳过视频 worker，也不在原厂/UI线程阻塞。
2. 原厂 GO 106 completion 发布 `go[7]` ACK。只有自己的 ACK、原厂 result +6 成功、driver state 5 三者齐全，worker 才解除 codec 静音，不再等首个 500 ms 静音块。codec I/O 不放在 native callback 中。已 ACK 的失败返回明确 error，stop 保留资源到 ACK/idle/callback drain，并退避 2 秒重试；不会永远把失败当 warming。
3. 解除静音时丢弃已完成块；第一块的原有 timestamp 算法若跨越 unmute 时刻则丢弃。`frames==0` 时 `anchor_us` 是有效起点门槛，之后恢复原来的 sample clock anchor。overrun 清除未冻结历史，已冻结 mux 数据保持不变。
4. `audio_warm` 填在原 runtime alignment padding 中。Audio On 成功打开但没有 PCM、没有完整有效历史，UI/快门 companion 接受条件均保持 Preparing。历史必须覆盖当前视频将选取的 IDR，并有新鲜 PCM。Audio Off 或明确已失败且资源已清理，才沿既有 video-only 策略降级。当前无 ADC 的 QEMU 不再能凭空算 Audio On ready。
5. freeze 不再接受 `clip_start` 早于音频历史并补上整段零样本。窗口缺失时返回 GAP，按既有等待/skip 策略处理；不会交付“开头主要由补零组成”的伪完整音轨。声学上的真正静音仍是合法 PCM，不用幅度阈值判 ready。

这消除了已证实的启动时序/补零通路，不是 9060/9062 真机根因已唯一确认或声学修复已通过。callback timestamp 本身沿用现有近似第一样本时钟，未实测 ADC 硬件 latency。快门暂停/恢复尚由 portrait 独立实现，合并后须重新审 bridge 与 Ready 条件。

## 验证

- `startup-tests/run.py`：实际 core/backend C 与从 runtime 原文提取的 audio_step/ready_step，ASan+UBSan，10 组 PASS。包含无 PCM warming、GO/codec 失败、有效边界、1.5 秒 worker 延迟、整段缺历史、frozen mux overrun、正常安静声音、late GO during STOP、五种已确认 native GO 失败及退避。
- 旧 `feat-v52/audio/test_lifecycle.c` 链接最终候选：4 组 PASS，涵盖 partial start、stop ACK、callback drain 和关闭后 callback。
- `startup-tests/test_native_go.py`：精确 G SHA ed1e8496…aa613；原厂 dispatcher/GO 六种情况实际 AArch64 执行。codec/DMA leaf 是明确 mock，不宣称硬件音频通过。成功 callback 见 state5；拒绝 state1/5 result0；三个内部失败 result仍1但 state4。`+7` 始终未被原厂改动，外侧 canary 完整。
- 同测试实际执行内核等优先级插入，两种 queue 模式都 GO 在 STOP 前；原厂 40c00b70 单 handler consumer 实际执行到 STOP 入口前已完成 GO callback。GO callback 是原 dispatcher 的同步 tailcall，STOP 处理不能先行。新会话只在原 stop ACK/idle/callback drain 后重用消息并清 +7；没有新增 callback 延迟生命周期。
- `startup-tests/compile_size.py`：AArch64 strict compile PASS；sizeof(runtime)=23264、sizeof(backend)=744，audio、audio_z8、audio_retry_us、flag_req、crumb_head 全部原 offset 不变。
- 当前三个对象总增长 968 B，其中 poll 716 B 放入 `.live_extra`，主段净 +252 B。**这是对象统计，不是最终整合链接预算通过**；与 EVF/shutter 额外段统一排布由 root 完成。

无物理 mic/扬声器测试；没有新整机音频 QEMU PASS。既有 v53C REC PCM/descriptor 与 TX 模型边界在 `RESULT.md`，不能借用成新 capture 源的整机通过。

## 配置范围

当前卡配置 `live_config.h` 只有 Live requested 和 shutter mute，没有录音 Audio Off；`live_tune.audio` 默认硬编码 1，无 UI 或文件 setter。因此不能把 mute=1 当录音关闭，不能用 RAM 修改默默绕过新增音频 Ready。缺 PCM 的最终镜像 core 回归应预期 Preparing；若要测试视频拍摄链，需明确独立 Audio Off 构建或确有输入的模型/fixture，并注明不是最终镜像的 Audio On 声学验收。

## 结合版审查状态（后续）

portrait 691fb636 的独立 PCM/Ready 四组测试在 `combined-tests/result.json`，范围仅包括两次明确暂停的音频窗口、冷热 Ready、前缀缺失和 frozen reader。**该版随后被 playback 找到 partial-resume + 并发新 held + cancelled 的跨 adapter 状态机死锁，因此不得凭四组 PASS 交付。** 修正后的 STARTING 清理路径正在加入 actual adapter+backend 联合反例；不把孤立后端测试扩大成完整调度通过。

修正结合版 de9f3c85 的联合反例与有效性反证现已完成，见 `combined-tests/REVIEW.md`；只代表该明确 C 状态机路径，不替代 native producer 顺序和物理声学测试。
