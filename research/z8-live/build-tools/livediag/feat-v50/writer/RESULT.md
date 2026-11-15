# 最终 writer 候选：v51d 集成输入

正式重放顺序：v49 源应用 `writer.patch`，再应用 `nctg.patch`，然后完整重建 Live 与 playback。后者 SHA-256 为 `94f00a7ae315100c8a3a464e9dabfcc7a0afad77f261ab189971ca01cd2531d1`，6 个源文件的前后 hash 在 `nctg-changes.json`；最新源码在 `nctg-work`。其中 `live_w5enc.c` 只有固定 FPS 数字换成共用常量，应保留集成分支已有的资源让路修改，不能用本目录的整个旧基线文件覆盖它。

早期 `host-sample.MOV` / `writer.patch` 的七条 NCTG 和 4:2:0 NCDT 图只通过低层 parser，完整播放器发现两处额外问题。它们已由本次增量替代，不能单独作为最终交付。

## 实现

MOV 添加 `niko`、每帧规范为一个 AUD，按同步帧分块，使用 `stco`。当前 HEVC 固定15帧 GOP，末块可短。音频以48kHz双声道 packed LE24 写入、每块最多24024帧；原 s16 左移8位可无损还原。Apple content identifier、UUID 与 timed `mebx` 均保留。普通变长 GOP 的 AVC 表仍为合法 MOV，但不承诺通过原厂固定 GOP 限制。

NCDT 从本次片段的中间预览帧动态生成四图：NCTH160×120、NCVW1280×720、NCM1640×360、NCM21920×1080。保持画面比例，黑边填充。NCDT 专用 JPEG 改为4:2:2、JFIF和独立表标识；原 Live 预览仍走4:2:0，其随机噪声帧输出与旧版逐字节一致。四图从现有1024×682 I420逐块采样，不申请大尺寸 I420，沿用64KiB I/O缓冲和3MiB输出上限。

NCTG 共19条，仅写厂商/机型/软件、当前视频与预览尺寸、真实帧数、当前编码 profile 的名义 FPS、格式、实际音轨事实。Wave5 的 AVC/HEVC mode entry 都使用30000/1001，新常量同时供编码配置和NCTG使用。音频写成功才声明PCM/2声道/24位/48kHz；无声或读失败回退时四项全部0。不复制旧日期、镜头或 donor 图像。

时长需区别：NCTG以真实count和nominal FPS描述编码配置；MOV `stts` 保留实际逐帧时间，VFR时两者可不同。没有从低帧率现象伪造慢动作标志。QEMU 的 `audio_output none` 不能证明真机出声或声画同步。

实际无音轨曾触发第三处问题：原厂 parser 在 timed metadata 之后再遇 udta，会沿用 metadata track kind 并返回6。新增 transaction 专用 `live_mov_finish_prefix`：先输出 video、成功的audio，流式写NCDT，再把已保存在小atoms缓冲里的Apple root metadata和timed metadata轨放到moov最后。随后回填最终moov长度。普通finish仍一次完整输出。没有搬动大udta、伪造音轨或改宽原厂classifier；`mebx`样本仍在mdat原位置。

预览声音 `LVJB` 保存s24引用，reader兼容旧 `LVJA` s16，播放器转回原厂s16环。最大末帧JPEG重试后的padding位于声音记录之后。流式预览写回检查取消，NCDT到metadata尾部之间再检查一次；失败删除本事务MOV，不留MAN。

## 验证与容量

最新完整C事务样本：`nctg-tests/final/host-sample.MOV`，3066810B，SHA-256 `84d3f5d81d42c91973467e8a2f0b3a904338f5f7fd421b6dbca33b5e9960bb39`。它使用真实95帧HEVC、162287帧s16音频及该片自己的预览帧；没有外部 donor 像素。

- ASan/UBSan完整事务、三种原有失败/取消回滚通过。新metadata尾部写失败、moov长度回填失败也删除MOV。
- 无音轨、首次音频读取失败、已写一块PCM后下一次读取失败，都成功产出无soun轨的MOV，NCTG音频字段全0。前两者输出相同SHA `a5a3995963bc4b2ad57280c60927da620344d18ac2876fe4b9e2741a22e05b99`；部分回退为 `5fb78b2d7a6371038728901a30a7804093cac4c9e7843aa6c2bf6f478e98940d`。
- 视频VCL、Apple标记逐字节保留；GOP块为15×6+5；PCM块为24024×6+18143；s16→s24→s16完全一致。
- 原普通420 preview在1024×682随机噪声上与旧encoder完全一致（397359B）。LVJA/LVJB、音频offset、输出buffer、JPEG上限、scratch过短拒绝和大末帧重试均通过。
- 新四张C422 JPEG经实际mvq JPEG model的header与完整decode全部通过，format=1。独立reviewer另用四象限色度图、ASan/UBSan和djpeg验证422纵向采样，16个内部RGB采样点误差不超过8。
- 实际原厂header IRQ指令已独立证明：请求422/输入420发0x2800且不启动image；请求422/输入422发0x2000并启动image。故问题是播放管线请求与输入格式不符，不能说JPEG库不支持420。
- 最新4个改动C源AArch64 freestanding编译通过。单函数stack：NCDT688B、JPEG1424B、write_pair832B、finish176B、extend48B；不是调用栈总上界。
- 相对v51初版，live_mov21464→21472B、transaction workspace87056→87064B（仅+8B）。原I420和I/O容量未增，集成必须重新生成runtime offsets。
- 独立 reviewer 的10路ASan事务全部通过，包括NCDT全部输出后触发取消（后续写入0次）、metadata尾部写失败和moov长度回填失败，均回滚且不留MOV。原厂 playback 的真实reader/audio_begin经host设备地址适配后复测LVJA/LVJB、跨块、2.4秒截断、零填充、读失败及边界全部通过。
- 最终4类C样本及reviewer独立生成的3类样本，共7个MOV经真实C3.11 ARM parser加窄meta hook执行，全部返回`[0,0,110933,0,1,0]`，四张NCDT图查询全部成功。query0/file-info的音频标志分别为有声1、无声/回退0，nominal FPS与codec正确。证据在`../audio/writer-review/audio-paths/{transactions,final-parser,audio-integrity}.json`及`../audio/writer-review/pb-audio-final-v51d.json`。

## v51d 集成输入核对

已独立读取远端`/path/to/user/v37build/v51d/work`，6个最终源文件与`nctg-changes.json`的after hash全部完全一致。最终MAIN的SHA-256为`aa6e94e0042fb445c666de73c0f6b9e7e569010b7993a3ac3da0979b174a9433`。同一镜像的548处竖屏矩形和124B直方图代码全部匹配8357634集成计划，两张完整OSD表及直方图区也与前一集成版逐字节一致。记录：`v51d-source-portrait-audit.json`。整个镜像、字体与其余overlay清单由独立playback Agent另行审查，未在此重复归功于本子任务。

## 原厂与整机证据边界

初版低层MOV/NCDT/GOP指令回放通过，但完整UI拒播；新旧JPEG/NCTG诊断分界见 `NCDT-NATIVE-INVESTIGATION.md`。最终四类样片的真实parser与file-info输出已由独立audio Agent核对通过，完整v51d拍摄与原尺寸native播放由主Agent并行运行并汇总。这里不提前把低层解析、Pillow诊断或4K替换样片等同于最终相机拍摄片完整回放通过。

原厂接受Apple meta handler仍依赖主Agent的窄hook；这里不改变DCF登记/相册入口，MOV仍写入原Live目录。未上卡、未刷机、未作真机播放、出声或同步声明。

## 最终整机结果补充

主Agent已用v51d实际产出的MOV（5bbc309c…da006d7，77帧）逐字节复制至隔离虚拟卡，原厂查询与解码到首帧后停于既有0x15421104显示模型缺口；同firmware的最终C有声容器+已知4K视频fixture完整95帧/11LPCM。具体loader与样片hash、未放宽的unknown失败见parser/final-native-results.json。

后续同一固件开启QEMU既有 `z8-heif-secondary-stop-model=on`，完整Live拍摄→45帧REC预览→退出恢复通过，IRQ1582→2078、编码413→659，最终707，原厂错误历史为空。固件、输入、deadline及断言未改；另增加被动观测点。双路40040通知发生在较早的菜单阶段，后续回放mask40成功；旧失败轮没有直接采集失败mask，不能宣称所有历史A2唯一根因。证据与固定重跑入口见 `../playback/A2-MODEL-AB.md`。

当前 `Z8-v51-Portrait-LiveMOV-aa6e94e0` 包标记 `core_regression_passed=true`、`release_status=qemu_test_candidate`、`physical_camera_verified=false`。原Lab包与旧失败记录原样保留，二者固件字节相同。本次通过不改变完整原尺寸原厂播放、真机音频/快门声及竖屏UI尚未验收的边界。
