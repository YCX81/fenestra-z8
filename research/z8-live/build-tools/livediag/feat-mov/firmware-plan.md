# 固件修改方案——未应用

> 本页记录 11 月 9 日提出的固件实施方案。当时的“缺 donor / 缺模板”是该阶段的样本条件。11 月 10 日已从历史 QEMU 产物取得完整 NCDT，并实跑当前 MAIN 的解析入口；仍缺在这台 Z8 上确认可播的实机正对照。[更新的 NCDT 证据](FIRMWARE-NCDT.md)

目标：最终只保存一份用于原厂全屏播放的 Live MOV，保留 still 关联、徽章和退出恢复；在离线文件通过原厂播放器之前，保留现有 LVP 功能。当前没有修改 C 源码、补丁脚本或固件，没有编译可刷镜像。

## 1. 先固定 writer 格式契约

`src-fix31/codec/live_mov.c`：

- `live_mov_begin()`：头部 28→32 字节；ftyp 长 20→24；保持 major=`qt  `、minor=0、compatible[0]=`qt  `，在文件偏移 20 放 `niko`；mdat 起点 20→24，payload 28→32。
- `live_mov_finish_with()`：当前 `put(len,m->pos-20)` 和 `writeat(m,20,...)` 必须一起换为保存的 `mdat_start`，不能只修改 begin。所有视频 offsets、PCM 起点从实际 `m->pos` 记录；加入溢出检查，当前 writer 本来就限制 uint32 文件位置。
- 拆分 `metadata()` 中“moov 级 mdta UUID”与“trak/meta/mebx still-image-time”。相机模式不写 metadata track、其 9 字节 sample、tref/elst/sample tables；保留 moov 级 UUID 作为独立待测变量，不能把删除 mebx 等同删除所有 Apple metadata。保留 track ID 1 和音轨 ID 3 合法，mvhd next_track_ID 仍须大于现存最大 ID，不强行为连续编号。
- 将 `press_us - first_pts`、命中帧的开始/时长、UUID、方向等保存到有版本的 manifest，供日后 Apple 导出。去掉 mebx 后不能丢失这个时间基准。
- 首个 SPS 获取后解析真实 coded/display size（包含 crop）；容器 avc1/tkhd 以及方向矩阵的平移使用同一显示几何。本样本是 1024×688；这并不纠正编码器的 padding 像素，只让描述一致。若想继续显示 682 行，须另行证明可正确设置 SPS crop（4:2:0 逐行的裁剪单位为两行）并验证解码，不能这次顺手改码流。11 月 10 日的 09 实验件已在离线 remux 路径验证底部 crop=3，并解码为原有效 1024×682。计划移植 writer 时仍要验证真实输入、位流与目标播放端。[09 文件与验证](README.md)
- Nikon udta 采用经过样本对照确认的生成模块；当前缺完整内容模板，**不要在生产 writer 填入本轮 66 字节 shell**。写 moov 前计算尺寸/容量；当前视频 writer 是先写数组后检查，增加 NCDT 时要改为写入前边界检查，不能仅扩大固定缓冲假设够用。

`src-fix31/audio/live_audio_mov.c`：

- audio `co64`→`stco`，单项 offset u64→u32；转换前检查 offset 和整个 chunk 末端不越过支持的文件范围，超限返回失败。保留 sowt、48 kHz 双声道 16-bit 和现有 timing；没有依据改 AAC。不要只改盒名而留下 8 字节项。

`src-fix31/codec/live_transaction.c`：

- 给 writer 传入明确 profile（camera / Apple export）；保持 MOV 关闭成功后才提交 manifest 的事务次序及取消/失败清理。
- manifest 增加版本、MOV 实际路径/对象标识、press 时间相对值、方向、音轨状态。UUID 与原 still 一致；旧 manifest reader 明确兼容，不把新字段误判为损坏。
- 最初保持 LVP 路径，通过文件实验和生命周期验收后再删除。目录内相同编号、不同前缀会冲突；相机 MOV 如果进入 DCIM，应使用唯一的已分配编号并走真实登记协议，manifest 记录 still 与 movie 的关联。不能假装 `LIV_0123.MOV` 与 `DSC_0123.JPG` 会自动成为两个对象。
- 任意路径播放 ABI 未闭合；`0x40cf3ffc` 是复制 0x48 字节结构后启动流程，不是可直接使用的 `play(path)`。先确定完整对象结构、消息、返回回调；若只支持登记的电影，则唯一 MOV 放 DCIM，避免继续产生 LIVE 与 DCIM 双副本。

## 2. 播放通过后，删除 LVP 和 inline 路径

| 位置 | 计划改动 |
|---|---|
| `codec/live_transaction.c` | 删除 preview_sink、preview_frame/put/yield、LVP create/write/close、ownpv 和对应回滚；保留 JPG/MOV/MAN 事务。live_data_paths 拆成 manifest path，旧文件清理单独保留。 |
| `codec/preview_writer.c/.h`、构建清单 | 移除新生产文件的 writer 和链接依赖。 |
| `capture/live_stream.c/.h` | 移除 `(LIVE_PREVIEW_SLOTS+1)*LIVE_PREVIEW_FRAME` 分配、切池、ring、PTS、preview_push、live_stream_preview_frame；重算后续布局和对齐。`preview_pad` 被 generation 重试逻辑复用，保留或重命名，不随 preview 字段机械删除。 |
| ticket、workspace、`runtime.c` | 移除 preview 回调、scratch 与初始化/传递，保留压缩 AU ring、I420 编码输入以及 photo capture 所需缓冲。 |
| `pbfix/live-playback-ui-icon/runtime.c` | v32 指向 icon 版本。替换 start/frame 的 LVP reader，去掉 preview fd、I420 和全帧 RGBA；改成启动 stock 全屏播放与退出恢复。不要只修改旧 v28 副本。 |
| icon 的 `preview.c/.h`、`ui.c` | 删除 LVP header/frame parser 和 pb_scene_frame 全帧绘图；徽章绘图保留。 |
| icon 的 `sync.c/.h`、`native_sync.c` | 新 schema 的文件组不要求 LVP；旧 schema 仍识别 LVP 的删除/只读传播。谨慎处理 LIVE_FILE_COUNT 与枚举索引，MAN 最后清理。 |
| icon 的 `native_ui.c`、hooks | 保留徽章 attachment/detach 和页面恢复；只移除已确认只服务 inline 播放的部分。 |

预计避免每次约 47,139,872 字节 LVP（源码格式算术，非本轮硬件测量）。捕获池请求可减少约 53,425,152 字节，但实际池粒度、stock decoder 新分配与运行时净收益仍须测量。

## 3. 生命周期不能通过删文件解决

进入 stock playback 前需要冻结新 Live 编码、等待保存/在途工作完成、执行已证实的归还流程并验证成功；失败留在 still。保存原 still ID、页状态与卷 generation，退出后待 stock 完全停止再恢复 Live。覆盖取消、换卡、删除、快进、重复进入、播放异常等路径。

必须尊重 v32 的模式切换历史：`v32_patch.py` 移除 worker 在 photo→movie 切换时主动 close，由 bring-up hook 交还。不能为播放重新加入未经验证的 close；也不能因删除 preview 而释放 Wave5 arena。播放专用 handoff 尚需闭合，这是实现阻塞项，不是已解决 API。

## 4. Apple / iPhone Live Photo 兼容性

【源码证实】mebx 承载 `com.apple.quicktime.still-image-time`，moov 级 mdta 承载 `com.apple.quicktime.content.identifier`，JPEG 写入同 UUID。删除 mebx 会丢失 timed still marker；即使 UUID 保留，也不能继续保证 iPhone 将这一 MOV/JPEG 识别为完整 Live Photo，按压时刻也不再可从该轨恢复。本轮按用户约束未联网核对 Apple 文档，也未做 iPhone 导入实验。

方案：相机只存 camera MOV + 配对 still + 小 manifest；在电脑/手机导出时无重编码地生成另一份 Apple MOV，恢复 UUID metadata 和 mebx still-image-time（含准确 edit timing），同步更新 offsets，与 JPEG 同 UUID 导出。可同时移除 Nikon udta 并回到已验证的 Apple 容器设置，另做 iPhone 导入测试。不要把机内唯一 MOV 原地换成 Apple 版本。保留原始 Live MOV 也是当前阶段的恢复依据；本轮工具不实现 Apple 反向导出。

## 5. 最终补丁的实施方式与验收

按 `v32_patch.py` 风格，在另建的源码副本上用精确 old-string + `count == 1` 断言生成补丁；对输入源码 SHA/version 和输出差异做检查，失败中止，不模糊替换。先独立提交 writer 格式，再做 stock 播放入口，最后删除 LVP；这份方案没有应用补丁。

验收：writer 产物需通过本轮解析器与 ffprobe/实际解码；故障注入覆盖写满、取消、音频失败、关闭失败和 manifest 最后提交；再由用户做卡片原厂播放验证。之后才测试 Live 启用后的播放/退出和资源归还，最后验证 Apple 导出。相机结果未取得前不能声明 47 MB LVP 已可安全删除。
