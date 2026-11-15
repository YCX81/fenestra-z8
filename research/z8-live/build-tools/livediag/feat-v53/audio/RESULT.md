# v53 REC 音频候选：独立证据与限制

用户真机反馈是 v52 的 REC Live 预览仍无声；Live On 下原厂测试视频已有声音。新拍 Live MOV 的原厂音轨尚未由用户验证。这里修的是 REC 输出链，不能据此推断麦克风录制质量、快门声或真机已经可听。

早期扬声器候选只改 `work/pb-fix37/runtime.c`，SHA-256 `96cff4d12f9652effbbaf7ff1f690a85cdbc5a95b2bac4f0894e4c4878674daa`。`speaker-output.patch` 相对于 v52 G；`changed-files.json` 固定旧/新 hash。保留 v52 handoff、ACK/取消/迟到回调处理与所有 W5/CFR 修复；无结构 ABI 变化。

## G 同镜像观察复跑：完整声画与返回 Ready 通过

`evidence/final-g-gui/final-summary.json` SHA `9a962b45512f3af0be23808b06cc752bbeb4cf3d4b6356f60831c8f66d25027e`：MAIN仍`c1489408…`、model仍`0f341f32…`，只增加只读GUI/布局/状态观察，原触点、样本时钟、固件门槛和60秒Ready截止均未改。runner **3210项、error null、unknown空**。四个关键结果是实际收据：

- 新MOV `1b817607…` 的145745 stereo frames /582980B PCM全部完整解码；无开头/末尾静音，长零仅150.020833ms曝光duck和87.125ms暂停恢复桥接。拍摄窗口及整轮无外部输入gap。
- 两个音效都实际消费4800B，分别与已知tone1 PCM逐字相等；第二个来自明确synthetic resource4e。两次自然完成，取消/错误为0，不能据此声称真实tone2音色或音长已验证。
- REC实际4个192000B块，前582980B与完整源PCM逐字相同，余185020B全零。音频真实截止3036355us，实际STOP3038001us；STOP/ACK后audio_stage0/audio_gate0。新preview为45×67475us=3036375us，GUI成功present的实际索引完整为0到44，45queued/45rendered、last44、decode/ui错误0，随后自然EOF。
- 原半按/释放返回view3/state1，最终requested1、capture.phase2、audio_warm0、audio.active1、audio_z8.phase2、encoded1071，满足可继续拍摄的Ready。

整段模型TX/IRQ、原固件服务和PCM字节验证不代表物理ADC/DAC或真机已可听。原始证据3223个文件及逐项hash位于该目录，归档`evidence/final-g-gui.tar.gz` SHA `490ceed0d50067f3064e4b350a98df8757a54bdcac8e589d206d2b00260923af`，remote/local一致。

**前一轮同G出现单帧GUI/待机失败，根因仍未定位。** 新观察轮通过只能说明本轮全部门槛满足，不能称已经修复GUI偶发；下节失败及所有旧收据保留。F配同v3模型的独立对照也通过GUI/返回Ready，故不能仅归咎v3。最终独审/打包必须同时带这项限制。

## G 前一轮失败：音频完整，整机 GUI/返回 Ready 未通过


整合 G MAIN `c14894087d5fd03b0124c7d94f6cd521e94def58831a087738145ef91b60fcd2`、严格私有 counter-clock 模型 `0f341f32…`。`evidence/final-g/final-summary.json` SHA `1ed357be8e90c723a76c06df85cea67be44a061e82285d6547b7df7484fb8d6e` 保留3385项check成功和整个runner失败：**REC后60秒内未回到真实Ready**。异常文本中的“no FULL input sent”来自复用等待函数；该轮实际上已拍摄、保存和REC，不据此否认已有拍摄。

实际MOV SHA `340fbc5a…`、1846481B；153754 stereo frames / 615016B PCM，起始与尾部零帧数均0，视频/声音完整ffmpeg解码无错。新LVJ1为45×71183us=3,203,235us，对应实际clip。真实ring含完整PCM，4个REC DMA块导出768000B，其前615016B逐字匹配全部源PCM，之后全零；两个4800B音效均自然结束，cancel_posted0。这里依赖明确synthetic tone2与严格私有RX/TX模型，不表示物理ADC/DAC、真实tone2音长或声学已验证。

**该轮只有1帧queued、0帧实际rendered，45只是文件帧数。** `rec_video_sequence_passed=false`、`returned_to_audio_ready=false`，所以音频子链成功不能覆盖整轮失败。旧observer没有保留last_frame、GUI gate和native view，不能从旧menu.page字段判断是否退相册；后续截图/MCU证据显示半按已被原厂消费。最终requested0及CPU reset/park需要结合真实event7追查，不能归咎麦克风仍占用（实际active0/phase0/stop_ack1）。只读GUI诊断已在独立同G、同模型、同触点/60秒门槛的lane重跑。

同G播放未修改的旧F counter片：`evidence/legacy-g/legacy-summary.json` SHA `b6e7985e5baa1b717e4046ecfae902412692618ee7059a01a862709c38136889`。原45×66667头与147347-frame PCM保持原样；768000B实际TX前589388B精确匹配完整PCM，自动EOF/STOP ACK后stage0/gate0。旧observer观察31帧queued/30 rendered，但缺首尾index，完整视频gate仍false。原40秒runner235/236，仅此前的清理后静置`0d154340`未建模写，保留失败，不改旧runner或容忍范围。

最终G实际C reader/decoder的离线ASan/UBSan验证在`evidence/final-g/jpeg-review/result.json`：新MOV全部45个JPEG均成功解码。这排除该样本必然损坏，不能证明GUI线程在fullcapture现场完成呈现。已完成的失败收据原版均保留`.pre-video-gate`，补门槛后的summary不会被当作交付通过。

## F 历史验收：完整流程跑完，但发现尾音截断


`evidence/final-counter/final-summary.json`（SHA `01546f884d9232646c24272628e93cb057b69bee2c24baef49a31d925c315083`）：exact F、严格私有模型 `5fb67d5a…`、明确 synthetic 25ms tone2 resource。runner 3147/3147、error null，拍摄/保存/REC 自然结束/半按返回全部走完，最终 audio_warm0、active1、capture.phase2、encoded1074。两个快门音均自然完成，cancel_posted0；实际模型导出的两段4800B逐字等已知 tone1 PCM（第二段来自显式 synthetic resource）。无物理声学或真实 tone2 音长结论。

实际自产 MOV `7dcea6e0…` 含147347 stereo frames / 3.069729秒。视频和声音完整 ffmpeg 解码通过，起始/尾部零帧数均0；仅150.020833ms曝光duck和48.3125ms暂停恢复间隙。有效拍摄窗口无外部供料gap，停止时1次尚未消费的RX输入被严格取消；全程没有guest动态RAM修改。

**但固定 LVJ1 为45×66667us=3.000015秒，`frame()` 在图像EOF立即 `hide()`，它会停止还没播完的音轨。** 本轮960000B真实ring含完整589388B源PCM，实际TX只导出其前576000B，剩余13388B没有消费收据。模型以整DMA块导出，不能把它直接换算为物理FIFO少播的精确字节数；真正的产品缺陷由实际C反例独立确认。`rec_pcm_dma_passed=false`、`all_required_passed=false`，保留F全轮，禁止以runner绿灯替代音频验收。

最小PB修正独立于冻结F：`duration-work/pb-fix37/runtime.c` SHA `fba498cb737c62c7310cf39365942e40755f8cd4593d9e131bdc42aa4f5a68ee`，`duration.patch` SHA `7bee2363cb8372e17f20088fb6770e548d0970ce96a5dffd6f84868f57daa991`，无ABI变化。旧预览保持原图像cadence，EOF只在音频确已成功播放且未取消时保留末帧至实际PCM截止；START ACK统一声画起点、按最后sample向上取整截止。有效音轨超过当前4秒上限明确跳过（reason8），不再暗中截成4秒。新文件的准确预览时长/取样由portrait独立writer候选负责。

`test_pb_duration.py` 实际frame/audio/index的21组ASan/UBSan通过，冻结F在147347帧尾音反例失败；扩展`test_pb_audio.py`通过LVJA/B、4秒边界/超限、读取失败/取消/迟到ACK；playback独立217组复核通过。待完整整合新G与新自产/旧文件实跑，不提前记修复验收成功。外部TX模型的QOM/guest时钟差由独立模型修正，不给产品deadline填假余量。

当前实体卡只读结果已更新：双槽gen22/23均 `live_on1/shutter_mute0`，菜单为 **Shutter sound: On**。早期gen21/mute1仅为旧快照，不能据此归因用户设置；未操作卡。

## F 整合回归现状（最终镜像 + 严格私有模型）

最终 F MAIN `b0cd7c40c3c6ffdf07b87fcbab20b98fbe9ed0c2a848550855d989b9e64fd8b0`，ABI23744。原 public 模型的首次 REC 实验未通过：stage7等麦克风释放超时；只读 observer 确认 worker 停在原厂 `40c7fd08(0x15)` SPI RX，poll冻结而IRQ继续。旧G无PCM时尚未进入解静音读寄存器，不能以旧G的成功归咎F handoff。历史失败完整保留于 `evidence/rec-f` 和 `evidence/model-boundary-f`。

同一 F 使用隔离的严格 SPI v2 模型（QEMU SHA `9919f3444011e5625a51c6f394630ba4217956f5994d79a91ce077882d249ac9`）复跑：只读回实际已写过的15/16，不提供身份或默认值；实际原生句柄10、CS bank5/pin13、16bit RX1、原厂真正发送的dummy0均核对。两次read、零reject使Live worker继续，**108 → 111 → 116采集停止、REC 14 → 17 → 1a → 15 → 16**均实际走完。没有改固件timeout、资源门槛或guest RAM。

`evidence/final-native2/rec-summary.json`、`rec-messages.jsonl` 和 `rec-result.json`：

- 实际读取152152 stereo frames/608608字节，274741个非零int16，960000字节提交缓冲与此前已核对PCM及零尾hash完全相同。
- 冷cache0/pref15，OUTPUT17音量15；BUFFER1a=192000×5；原厂request27/28的真实DMA均为192000字节、目标0e200104。
- OPEN到START ACK约23.4ms；25次present后3.266852秒开始STOP，最终plays1/errors0/reason0/stage0/gate0。输入只包含20s进入相册及26s按/松REC，**没有外部停止输入**。源码按elapsed时间挑选45张预览中的当前索引，过期帧跳过；25不能写成45张都显示。
- **完整runner仍235/236项**：清理阶段`PC08201164 → write0d154340=01ffffff`是唯一未知MMIO，不能写整机全部PASS。
- 16bit单FIFO104的TX模型仍未闭合，TXjobs/bytes=0。PCM读取、路由、DMA构造和服务停止通过；**不是DAC消费或物理可听验证**。

严格RX16另见 `rx16-model/RESULT.md`：22个ASan/UBSan门槛/反例和20次原ARM构造通过。最终F的native rdw输入lane已观察原DMA→IRQ→原回调的连续非零9600字节块，但该lane的视频源未被Live接受，不是完整Live拍摄。完整merged核心Ready→快门让路→照片/MOV保存正在独立执行；不得用音频协议结果替代这一整机验证。

## F REC 实际TX消费（后续私有模型验证）

同一F再用独立私有rdw模型 `c1e0b9b6f59e0cdd61d68c5b49eaef66278fed7dad6655b90e1453888989d036` 跑已知非零LVJB。该模型依据原ARM的双DMA/FIFO104构造只接受192000B、映射范围、非重叠、27/28交替bank；DMA后走原厂IRQ和refill，没有伪造service callback或改guest。**此轮不覆盖SFX4800B**。

`evidence/rec-tx16-f/tx-summary.json` 与 `actual-tx.pcm`：

- 实际模型消费4块/768000B，SHA `486122a8fff0e63257ed001b175e8dd8ed83bce28cfd31a1c62ea7cd71c52682`。前608608B逐字节等完整源PCM，之后159392B全零。
- 原厂IRQ产生后续描述符：来源依0/192k/384k/576k/768k/0，request27/28交替，均为FIFO104/192000B。原厂STOP后模型jobs保持4、pending0、fault空，PB plays1/errors0/reason0/stage0/gate0。
- 全runner仍235/236，仅退出静置路径`PC08201164→0d154340`未建模失败；不能写整机全通过。已有正常播放子链与这一失败分开记录。
- 模型显式使用48k stereoLE16功能时钟、首块1ms预填；这不是物理FIFO延迟、DAC或声学输出证明。host_output/dac_output均false。旧public/v2模型TXjobs0的证据保留，不回填成消费成功。

## F 完整拍摄：已保存，但尚未通过全部验收

`evidence/ready2-capture/identity-and-capture-summary.json` 独立审计实际 kernel 为上述 F、QEMU为私有merged `cb696539…`。原结果中的 `firmware_sha256` / readiness标签遗留prepared-stock SHA `2b43…`，保留原始文件，不把该标签当执行镜像；以实际command、launch前hash、manifest与身份审计合看。

外部driver在FULL前只读等待实际Ready，0.493秒后audio_warm0/active1、33个有效PCM块、66个编码帧，stock mute0/live mute0。原始触点触发后保存了原厂JPEG、96帧1024×682 HEVC MOV和112B MAN；transaction1/error0、audio_tracks1/skipped0，随后继续编码至encoded244/job0，但audio_warm仍1，不能写成已回到可再次拍摄的Ready。全部2386项原有check成功，但外部PCM feeder在保存后落后一个50ms输入块而主动终止，**整个runner仍失败**。Ready3追加外部服务机会后同样保存并恢复到encoded705，进相册时仍发生供料迟到；没有据此记REC完成。

Ready2实际MOV SHA `31b9e74204c25261149203e40463d4ecd605e73f83ffbf90510a9c8a1dc2f0fa`：视频3.203200秒、PCM153753帧/3.203187秒，全ffmpeg解码无错，峰值4000吻合测试输入×4。**起始无静音，但从1.633708秒至结尾1.569479秒全零**，故不能认定全段录音已修好。原始`capture-stages.json`显示第二SFX开始至麦克风恢复约2.032秒，输入PCM中断2.217秒；第二次实际BUFFER1a竟为bytes0/count1，随后STOP payload有04100032取消。当前shutter.completed2只证明包括取消在内的完成回调，不能写成快门音自然播放完成。后续原ARM独审已定位：tone2取resource4e、bank24 ROM46460000；当前loader缺这块，默认零，G/F在任何声音前就读到长度0。第二次完成包含2秒后的取消。即使本次尾零由缺资源解释，也不把它当成完整音频验证；下一轮须提供明确标synthetic的短音效并观察自然完成。未缩短固件timeout。

## 三个有反例支持的缺陷

1. **没有设置扬声器输出。** 旧 REC 仅发 `43180014 → 1a → 15`。原厂 OPEN 清空 driver output profile，仅当 cache 的 dirty 位为 1 才重套；之前成功 `17` 使 cache valid=1、dirty=0，所以 OPEN 后“命令成功、driver=3”并不表示有输出路由。候选在 OPEN 成功后发原厂 `17`，等它成功再给 buffer/start。`native-output.json` 实际原厂 ARM service/driver 反例覆盖 dirty0/dirty1；`evidence/native-messages.jsonl` 是未修改 G 原厂有声视频整机的真实 `17` payload，对应音量 15。
2. **原厂视频界面音量缓存尚未初始化。** 初版 A 从 `a033868d0` 取音量；首次纯照片 REC 整机实际读到 0，实际 `17` 三个音量也全为 0。原厂进入 MOV 时才从 `403ec398(0x02000110)` 装载这个 UI 缓存。候选直接用同一个原厂偏好 getter，其 ROM 表 `4400a908` 是 type2/u16，指向 `a03d47afc`。B 整机观测到 cache=0/pref=15，实际 `17` 音量=15；真实 pref0 仍为静音，不人为抬高音量。
3. **单块 960000 字节超出原厂 DMA 长度。** 原厂 `40c17954/40c17c48/40c17cf8` 用 16 位字段保存 `(bytes/4)-1`，旧单块实际截成 173568 字节。B 整机实到后两入口，故这不是未使用分支。候选保持同一 960000 字节预填缓冲，分成 5 × 192000 字节；最多 4 秒 PCM 后仍有至少 1 秒零尾。原厂 ring 本身轮换，候选不增加异步 refill、不扩大内存。

## 已执行检查

- `test_profile.py` / `profile-tests.json`：实际编译候选 AArch64 helper，与原厂 movie UI payload constructor 逐字节比较。完整原厂偏好 getter、ROM 表和读取指令实际执行；仅运行时 group-ready/type2 reader 分发表初始化。96 组（volume0/1/8/15/20/36 × 4 × 4 连接组合）通过，候选的 UI cache 每组强制保持 0。
- `test_pb_audio.py` / `pb-audio-review.json`：实际候选函数体 ASan/UBSan。LVJA/LVJB 完整 0–4 秒非零 PCM、24→16 位转换、读块边界/越界、零尾/容量、读取失败、mic 等待、OUTPUT 负回执/取消/丢 ACK、清理后新 owner、handoff admission 均通过。新增断言 service1a 为 192000 × 5。
- `test_serial_block.py` / `serial-block-tests.json`：三个未改原厂 ARM serial constructor 都实证旧 960000 → 173568、新 192000 精确；DMA install/start 是只记录参数的测试边界。
- `test_native_ring.py` / `native-ring-tests.json`：未改原厂 ring cursor → serial constructors 连续 10 次，paired/unpaired 两路；旧单块重复截断首段，新块地址依次 +0/+192k/+384k/+576k/+768k 后 wrap。仅 RAM aperture 翻译和硬件 DMA install/start 被截获；不是声学测试。
- 原厂 G 有声 MOV 观察 lane `/path/to/user/fxfix/v53-audio/native-route-v2` 完整结束，95 个 DEC_PICTURE；实际 `17` 非零 payload已保留。早期 v1 observer 因 x0 合法 handle 为 NULL 而没有记录，已单独保留为观察器失败，不把无记录解释为未执行。

## 非零 PCM 整机实验

独立普通文件虚拟卡包含实际 G 拍摄的 JPG，以及实际 C writer 构造、带非零 LVJB PCM 的 MOV。没有启用 PB_AUDIO_TEST，没有给 guest 写寄存器/动态 RAM，没有改主固件/仿真模型。仅此私有测试的 JPEG 预览间隔由 66667 改为 70441 微秒，使 45 张 JPEG 的窗口覆盖已有 152152 帧 PCM；该调整不是生产 writer 改动。`prepare_rec_card.py`、fixture manifest 和完整严格镜像 hash 留档。

B 提交给原厂 service1a 的 **实际 RAM 缓冲**已被只读 plugin 导出并逐字节验证：

- 152152 stereo frames / 608608 字节 PCM，274741 个非零 int16 sample；PCM SHA-256 `f651b8b3139702ca579a8bc35e23ea50d4ba23451bb67fe257b64ea5c426cbad`。
- 完整 960000 字节 ring SHA-256 `8e714ecfd7da3e589aba3995674afb803706142f03a2120aed14b5feee7c1150`；后 351392 字节全为零。
- 冷 cache0/pref15 → OPEN/OUTPUT/BUFFER/START 成功，audio_plays1/errors0，25 次 preview frame；结束 STOP ACK 后 stage0/gate0。
- A/B 旧单块不能作为完整音频验收。B 对读取、转换、冷音量和到达 DMA constructor 提供实际证据；C 才含五段修复。

C 镜像 `/path/to/user/fxfix/v53-root/v53c/q/main.main`，SHA-256 `4a24a4655c3afb14df4b89dd305bec62100030144ffca36157747d26f09019ce` 的最终整机 lane `/path/to/user/fxfix/v53-audio/rec-c` 已结束。`evidence/rec-c-summary.json` 与原始 `rec-c-result.json`、`rec-c-messages.jsonl` 是完整收据：

- 冷 cache0/pref15，实际 OUTPUT17 为音量15；BUFFER1a 为192000×5。完整960000字节实际RAM缓冲与上述PCM/零尾逐字节一致，hash相同。
- 原厂实际生成 request27/28 两个192000字节DMA描述符，source分别为`c70fe400`/`c712d200`（差192000），destination都是`0e200104`。
- OPEN→START ACK用时23387微秒，没有700毫秒重试。START ACK为26482249微秒；25次画面present后29753997微秒请求STOP；最终audio_stage0/audio_gate0、audio_plays1/audio_errors0。此计时来自固件自身计数器，不能代替真机首帧/声学启动延迟。
- 非零PCM读取/转换/提交和descriptor字段通过；full-native整体仍失败于下述未建模MMIO，TX drain未发生。

## 边界

当前 rdw TX 模型的具体拒绝门槛（按实际编译header重新核对）：`serial5_tx_drain_request` 支持FIFO104和13c，但request28只有serial bit1=1的paired分支接受。C/F实际原厂`40c18130`读取serial bit1=0选择目标`0e200104`，并提交request27/28；其中FIFO104的第二bank28被现模型拒绝；原生ring双DMA标志与FIFO位宽bit1是不同字段，不能混称单bank（`evidence/tx-model-boundary.txt`、`serial-target.txt`）。当时没有扩模型，C和最初F的TX jobs/bytes=0。后续最窄私有模型验证见上文；它执行原IRQ/refill并导出RAM消费，但物理节拍、DAC和扬声器仍未验证。A/B/C结束清理后另见 `0x0d154340` 未建模写（PC `0x8201164`, LR `40d6a70c`），因此 full-native runner 最后断言失败，不能写整机全部 PASS。实验不扩大 tolerate、不把未知 MMIO 当成功。

首次 REC 到声学输出、真机 PCM 采集质量/4 倍增益是否削波、物理扬声器与快门声仍需要各自真实观察。上述主张限于明确代码缺陷、原厂指令反例以及已有非零 PCM RAM/服务链证据。
