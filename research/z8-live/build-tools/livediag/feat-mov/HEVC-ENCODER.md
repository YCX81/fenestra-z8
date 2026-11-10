# 1280×852 拍照 Live 的 Wave5 HEVC 编码路径

**在固定的 C 3.11 MAIN 上，1280×852、HEVC Main 8-bit、12 Mbps 的参数能通过原厂输入检查，8-bit 源槽也够大。** 机身普通拍照档还缺连续的 1280×852 NV12 完成帧，以及以这种输入运行的 Wave5 参数集、压缩帧和交接记录。下面的机器码实验在 QEMU virt 上运行，未进入 Z8 OS，未启动编码器或连接设备。[取景源证据](LIVE-1280x852.md) · [探针](evidence/hevc-encoder/check_encoder.py)

## 原厂函数的实际结果

输入固定 MAIN SHA-256 `c7c6dbeea8a94064a10d47a7b9022ab26e4aa6b12ba5fb63b1ad85abac4fe4d4`。测试调用与现有 Live 一样的 `CONFIGURE(0x40c333d0)` 和 `PARAMS(0x402f6c10)`；前者用按 32 行对齐的高度，后者使用有效高度。随后执行 `SET_PARAMS(0x40c338ac)` 的原始检查至 `0x40c33d28` 之前。原有失败分支和错误返回保留，成功路径在获取 stream 锁之前停止。本实验中的“通过”只到这一边界。7 个有效参数 / 对照及一个非法裁剪字段均跑过。

| 模式表输入 | CONFIGURE 源 stride / 单帧字节 | PARAMS 宽×高 | profile 字 / level 字 | +0x68 |
|---|---:|---|---|---:|
| AVC 1024×682，8 位 | 1024 / 1,081,344 | 1024×688 | 0 / 42 | 0 |
| HEVC 1024×682，8 位 | 1024 / 1,081,344 | 1024×688 | 1 / 40 | 0 |
| AVC 1280×852，8 位 | 1280 / 1,658,880 | 1280×864 | 0 / 42 | 0 |
| HEVC 1280×852，8 位 | 1280 / 1,658,880 | 1280×864 | 1 / 40 | 0 |
| HEVC 1280×852，10 位 | 1728 / 2,239,488 | 1280×864 | 2 / 40 | 0 |
| HEVC 1920×1080，8 位 | 1920 / 3,133,440 | 1920×1088 | 1 / 40 | 8 |

表内单帧字节来自 CONFIGURE 输出 `+0x14`，stride 来自 `+0x1c`。8 位目标恰好能放入本版现有的每个 `0x195000` 源槽。10 位单帧为 `0x222c00`，并且当前代码只把 8 位 I420 写成 NV12。要探索 10 位，需要有证据的原始像素精度、打包方式、stride 与重新分配的槽。[当前编码调用、输入拷贝与槽布局](src-fix31/codec/live_w5enc.c)

12 Mbps 下 CONFIGURE 的 `+0x20` 为 2,250,000 B，正是 1.5 秒名义码量；当前表向服务登记两个各 2 MiB 的 BS 区并处理跨段。单帧暂存上限约 512 KiB，AU 环和冻结区各 5 MiB。参数计算器输出成功不能证明 BS 分段、峰值 AU、其他工作区或连续三秒均有余量。首轮 8 位实验应记录每帧实际长度、PTS 和 IDR 间隔，再决定码率或容量。[编码器固定区](src-fix31/codec/live_w5enc.c) · [AU 环容量](src-fix31/capture/live_stream.h)

两份原厂录像期真机快照中，`0xa02d30ae0 = encoder_instances + 0x1f0` 处的参数均为 codec 1、3840×2160、depth 8、profile 字 1、level 字 52、tier 字 1、GOP 字 15、裁剪四字全零。录像前后快照这些字段均为零。这是原厂 4K 录像的状态。[录像中快照](../../../audio-probe/logs/w5-h265-rec.json) · [录像中第二份快照](../../../audio-probe/logs/w5-h265-rec2.json)

### HEVC 的 profile、tier 与低层配置

原厂 `0x402f6c80` 和 `0x402f6e4c` 将非零 codec 的 tier 字设为 1。1280×852 Main8 用例实跑输出 `PARAMS[+0x20,+0x24,+0x28] = 1,40,1`，两次 4K 原厂实拍快照为 `1,52,1`。worker `0x40c3b530..0x40c3b54c` 在 HEVC 分支将 level 乘 3（Level 4.0 → 120，Level 5.2 → 156）。`0x40c3a9fc..0x40c3aa08`、`0x40c3b550..0x40c3b55c` 把 tier 从参数 +0x28 复制到编码描述符 +0xac。HEVC 路径的 `0x40c586a8..0x40c586bc` 将该字放进序列寄存器 +0x124 的 bit 12，`0x40c58708..0x40c58710` 提交值。原厂录像实际请求 **Main profile / High tier**，本例则为同组合、Level 4.0。快照本身没有返回 VPS/SPS。

这与播放端 `0x40ce8470` 的 `hvcC[1]` 条件对应。对于 profile_space 0，High tier 的 Main/Main 10 值为 0x21/0x22；host Main profile / Main tier 的 0x01 在该提取入口被拒绝。摄像机端 writer 应从编码器返回的参数集生成相符的 hvcC，并对 VPS、SPS、hvcC 的 profile/tier/level 做交叉检查。仅改容器字节的试验可定位分支，不能替代参数集一致的实验件。归档 QEMU 的 HEVC host 接口将宽、高、深度、帧率与 GOP 交给 x265，没有把来宾定义的 tier 字传给它；其归档 donor 也写的是 Main tier。[归档 host codec](../../../_mac-migration/live-session/live-photo-20261030/audio-qemu/src/qemu/nikon-wave5-codec.h#L34) · [归档 donor](../../../_mac-migration/live-session/live-photo-20261030/pair-u02a/DSC_0002.MOV)

### 852 行在参数、寄存器和 SPS 中的单位

参数函数会把 852 向上取整为 864。它仅在 `height % 16 == 8` 时写 `+0x68=8`，852 得到 0。另做一个受控输入，只在 PARAMS 返回后将 `+0x68` 改为 12。该值通过输入验证；传 512 则被原检查拒绝，返回 4。用当前 MAIN 的 worker 指针构造 `0x40c3aa84..0x40c3aac0`、复制 `0x40c3d028..0x40c3d064`、W5 寄存器打包指令 `0x40c58a64..0x40c58a84` 在两个私有缓冲上复演。四个参数字 +64/+68/+6c/+70 被复制到编码描述符 +d0/+d4/+d8/+dc。对 12 的实验，观测到寄存器 +0x134 的参数为 `0x000c0000`，+0x138 为零。对原厂 1080 样例观测到 `0x00080000`。这一步没有执行寄存器写、硬件编码或整条 worker 路径。[原始机器码摘段的驱动](evidence/hevc-encoder/params.S)

本地已有 Z8 QEMU 模型将 +0x134 高 16 位解释为底部裁剪**像素**，并显式拒绝 HEVC 的任何非零裁剪寄存器。这是模型能力边界。以 8 位逐行 4:2:0 HEVC 为例，若真实 VPS/SPS/PPS 给出 coded 1280×864、无既有裁剪，且有效画面确在顶部 852 行，可在照片用 HEVC 分支把 SPS 的 `conformance_window_flag` 设置为 1、`conf_win_bottom_offset` 设为 **6**，同时让 hvcC 与播放所用 SPS 保持一致；若码流本已声明 852 行则保留。还需初始化全部对齐 Y/UV 行，并覆盖 cache clean 范围。调整寄存器与调整输出 SPS 可做独立实验；能接受参数不等于 VPU 发出的 SPS 已正确。归档的主机 x265 样片只说明相应模型输出能够解码。[模型的裁剪判定](../../../_mac-migration/live-session/live-photo-20261030/audio-qemu/src/qemu/nikon-z9-wave5.h#L404)

### 实际把 864 行信令改为 852 行

新增的受限 C 函数 `live_hevc_sps_852(nal,n,out,cap,&written)` 处理单个 SPS（含两字节 HEVC NAL 头，无 Annex-B 前缀）。调用方须先确认顶部 1280×852 是完整有效画面。它限定 1280×864、Main profile、单时间子层、逐行、4:2:0、8-bit。无裁剪或全零裁剪时仅替换 conformance window，以正确的 RBSP stop bit 收尾并重新插入防竞争字节。已是底裁 6 单位时原样复制。其他已有裁剪、层/几何不符、空间不足都拒绝；输出和长度在失败时保持原值。它在有实际 SPS 后使用，不根据寄存器预测 SPS。[接口](evidence/hevc-encoder/hevc_crop_852.h) · [实现](evidence/hevc-encoder/hevc_crop_852.c)

验证从 90 帧合成 `camera-1280x852.MOV` 提取 Annex B，在桌面先将六个 SPS 全部去掉底部裁剪，裸码流逐帧解为 1280×864。用该函数修正后，所有 NAL payload 均逐字节恢复原值，包括 SPS 的 VUI、VPS/PPS 和 VCL；90 帧均解为 1280×852，与未裁剪码流的顶部 852 行逐帧像素哈希相同。另覆盖原地调用、已正确的 SPS、其他分辨率、左裁 2 像素、底裁 10 像素、截断、错误 NAL 头及容量不足。[可复跑的逐帧验证](evidence/hevc-encoder/check_crop.py)

## HEVC 参数集、取片与交接

1. 现有 `open_stream()` 在 CONFIGURE 的 `cfg[0]` 和 PARAMS 的模式表开头均给 0。HEVC 分支两处都需要 1，8 位时保持输入色度布局。原厂 HEADERS `0x40c33d98` 返回首地址、长度和四个长度字。worker 在 `0x40c3cd10..0x40c3cd3c` 写回四字；原厂 recorder 的 HEVC 分支 `0x402f49d4..0x402f4a48` 对四个长度各加 4，依次复制。归档模拟器为该接口提供 `AUD/VPS/SPS/PPS`。集成时按 HEVC 两字节 NAL 头识别 35/32/33/34，对每个长度、前缀、区域末端和总量逐项核实。当前 AVC 分支用 `nal[0]&31` 找类型 7/8，无法直接复用。[本版 open_stream](src-fix31/codec/live_w5enc.c) · [归档 HEADERS 模型](../../../_mac-migration/live-session/live-photo-20261030/audio-qemu/src/qemu/nikon-z8-wave5-headers.h)
2. 当前 AVC 路线在 frame_body 中把输出交给 H.264 slice 重写器。HEVC 需要根据实际 AU 中的 `(header[0]>>1)&63` 判定随机访问点，只有确认片头独立可解才向环标记。首个实现可限定低延迟 IP 顺序、定期 IDR_N_LP (20)。若允许 IDR_W_RADL (19)、CRA (21)、多层或重排序，则须一起处理前导图像、DTS/PTS 及 SPS 参数代次。服务 `TRIGGER` 的 w4 通过布尔校验再由 `0x40c39134` 发布到每流状态；w4=1 自身不能证明结果是 IDR。实机 AVC 已有强制 I 仍产普通切片的记录，HEVC 相同行为仍待实际 AU 验证。[实际帧回读与 H.264 重写](src-fix31/codec/live_w5enc.c) · [原有实测记录](W5-BRINGUP.md)
3. 用既有 30 帧 HEVC 样片实跑本版 `au_ring.c`。该归档文件 SHA-256 为 `e6ced75cca35ea4300e798782bbcda96e935ebba88983ad47f7f11dcda6b1971`，第 0、15 帧有 AUD、VPS、SPS、PPS 和类型 20 的 VCL，其他帧均为类型 1。把环限制为 123,716 B 后强制覆盖第一个 IDR，保留 29 个 AU。查第 14 帧找不到锚点；查第 29 帧得到 15，从实际环取出的 86,161 B 与原始 15..29 字节一致。FFmpeg 4.3.1 独立解码后缀得 15 帧，I420 共 15,713,280 B，SHA-256 `320e4de72a495abfda6d0a71c99359d36fd3312b41b299217bfcf2f2d19f7507`，与完整顺序解码最后 15 帧一致。这支持复用现有环；前提是正确标记 IDR，且可取到同一代的参数集。[环回放试验](evidence/hevc-encoder/check_ring.c) · [原始归档样片](../../../_mac-migration/files/_bundle-20261102/codex-tasks/w5-client/evidence/client-30.hevc)
4. 旧 `w5-client` 在 QEMU 使用直接 MMIO、自有 arena、host x265 CRF18；到目标 PTS 后关闭并重建实例，以使模型再出 IDR。这无法移植为实机每半秒开关 Wave5 的策略。当前 MAIN 的模式切换/录像/回放仍共享原有服务；无硬件完成证据时，不能发布资源已归还。另一条已存档的 74 帧、3.268098 s HEVC Main8/hvc1 是 1024×682 模型结果。现有 H.264 写出的有声 Live，以及桌面 1280×852 HEVC，可分别验证封装、配对与解码；以上均不证明无 HDMI 的 852 行机身生产链可持续工作。[旧客户端与 QEMU 的边界](../../../_mac-migration/files/_bundle-20261102/codex-tasks/w5-client/REPORT.md) · [实际交接顺序及反例](../codex-w5/W5-MOVIE-HANDOFF.md)

下一步能区分问题的顺序是先在现有 1024×682 ch1 取得真正的 Main8 参数集与完整短 AU 序列，验证独立 IDR、分段和保持原有交接，再接入已证明持续有画面的 1280×852 输入。验收要同时记录源帧进度、完整码流头、每帧长度/类型/PTS、实际解码结果及停止后服务状态。

### 离线复跑

从 `feat-mov` 运行。第一项需要 QEMU virt、clang 和 pyelftools。第二项编译的是当前源树的 AU ring，仅在临时目录生成程序和测试后缀；FFmpeg 默认通过 PATH 获取。
```sh
../feat-rec/.recorder-venv/bin/python -B evidence/hevc-encoder/check_encoder.py
python3 -B evidence/hevc-encoder/check_crop.py
work=$(mktemp -d)
sample=../../../_mac-migration/files/_bundle-20261102/codex-tasks/w5-client/evidence/client-30.hevc
cc -std=c11 -O2 -Wall -Wextra -Werror -I src-fix31/codec evidence/hevc-encoder/check_ring.c src-fix31/codec/au_ring.c -o "$work/check"
"$work/check" "$sample" "$work/tail.hevc"
ffmpeg -v error -xerror -f hevc -i "$work/tail.hevc" -vsync 0 -pix_fmt yuv420p -f rawvideo - | shasum -a 256
ffmpeg -v error -xerror -f hevc -i "$sample" -vf 'trim=start_frame=15' -vsync 0 -pix_fmt yuv420p -f rawvideo - | shasum -a 256
```
