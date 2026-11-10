# 照片档 1280×852 HEVC Live 的取帧、编码与播放

离线有声样片已保存 90 帧、3.003 秒、1280×852 有效画面。C 3.11 MAIN 可以打开相机格式的两份样片；读取 VPS/SPS/PPS 时，Main profile / Main tier 对照返回 4，Main profile / High tier 对照返回 0。画面用测试图生成，声音是 48 kHz 双声道测试音。尚缺普通拍照档无 HDMI 的连续 852 行真机帧、照片档 Wave5 实际产出的 HEVC headers，以及机身解码、退出后的状态。[Main tier 对照](evidence/hevc-852/camera-1280x852.MOV) · [High tier 对照](evidence/hevc-852/high-tier-1280x852.MOV)

## 先查 ch2 有没有真正收到两个平面的地址

在 SHA-256 为 `c7c6dbee…` 的 `bg34.main` 中，scaler 配置入口 `0x40cb7b54` 将配置对象保存在 `0xa02f26460 + 0x2d0*i + 0x2c8`，其中服务号 `i` 的有效范围为 0–2。配置对象的 `+0x90` 是 ch2 配置指针，`+0xc8` 可以指向启动时交给地址队列的数据。另一个入口可在运行中重交地址，该启动指针只能反映一次配置。历史 QEMU HALF10 记录过 `i=0`，当前机身需要独立辨认活动服务。[配置证据](../../../_mac-migration/files/_bundle-20261102/codex-tasks/photo-live/REPORT.md) · [HALF10 实例](../../../_mac-migration/live-session/live-photo-20261030/audio-qemu/src/docs/z8-1543-c0-stop.md)

队列提交函数 `0x40cb7650` 的条件可以离线实际执行。对于 ch2，`0x40cb97d8` 检查普通 RAM 指针 `0xa02f26cf8 + 0x58*i` 是否非零，再从本次提交块的 `+0x50/+0x88` 取 Y 和 UV 的四项地址表指针。服务依次处理 ch0 至 ch6，一个较早通道的登记和地址不匹配就提前返回。下面五组用原始指令、合成表和只在模拟器中存在的寄存器页运行。[复演脚本](evidence/check_ch2_queue.py)

| 合成服务输入 | ch2 四个 Y 槽 | ch2 四个 UV 槽 |
|---|---|---|
| 已登记、两表齐全，bank 0 | 写入 | 写入 |
| 已登记、两表齐全，bank 1 | 写入 | 写入 |
| 未登记、提供两表 | 未写 | 未写 |
| 已登记，仅提供 Y 表 | 写入 | 未写 |
| ch2 条件完整，但 ch0 已登记且两表都空 | 未写 | 未写 |

中断允许位另走 `0x40c25fac`，ch2 的普通 RAM 镜像在 `0xa02c97494`。ISR 将它与实际 B2 状态相与，有命中时才向上分发 `0x41070022`。现有 v19 的几何改写和 kick 不会建立上述软件登记或补齐提交表。以上是可能区分“尺寸写入成功但没帧”的条件，尚无故障当时的读数指明具体缺哪一项。

若以后做机身采样，可先读活动服务号、上述 RAM 登记/配置指针、ch2 IRQ mask 镜像，以及已经保存在 Live 普通 RAM 里的 `lv_geo[2]` 和 `lv_snap[2]`；同时记录照片模式、采样时间和 USB/LV 状态。所有指针在读取前核对版本、范围与归属，保留两次读数。要证明实际取帧，还需在取景运行期间对上 C0 的 Y/UV 完成代次和多幅变化的像素。USB 打断取景后读到的旧表无法提供这项证明。主机只读普通 RAM，历史上直接读 scaler MMIO 曾导致挂机。[既有机身观测](../codex-w5/history/hw-notes.md)

**ch1 也有一条更具体的路线。** 旧 QEMU 的 HALF05 快照中，class `0x0f` 的槽间隔为 `0x1c2000`，UV 起点比 Y 高 `0x12c000`，两组配置银行的四个 ch1 地址符合这个布局。以 1280 stride、852 行计算，Y 需要 1,090,560 B、UV 需要 545,280 B，在该布局分别余 138,240 B 和 69,120 B。另一份 run07 也观察到同样的六槽池布局。原厂显示消费者已被追到使用 B1 地址，更改 ch1 还需要同步显示侧输入尺寸/stride，验证实际模式下的槽容量和已提交 DMA 的回收时机。该容量计算不能外推至当前机身所有模式。[HALF05 实际槽表](../../../_mac-migration/files/_bundle-20261102/codex-tasks/photo-live/VERIFY-EVIDENCE.txt) · [run07 与显示消费者](../../../_mac-migration/files/_bundle-20261102/codex-tasks/live-b1-lease/consumers-findings.md)

## 把 HEVC 接到现有 Live 流

1. **输入与深度。** 以当前的 NV12、8-bit、4:2:0 和有效宽高为输入。实际执行 C 3.11 CONFIGURE/PARAMS/SET_PARAMS 的输入检查，1280×852、HEVC Main8 时 Y stride 1280，槽大小 `0x195000`，参数画幅 1280×864。10-bit 路径的 stride 为 1728、槽需求 `0x222c00`；复用现有 8-bit 槽会超出其容量。[参数与机身 4K 对照](HEVC-ENCODER.md)
2. **随机起播和环形缓冲。** 现有 AVC 的 IDR 修补器解读 AVC slice header。HEVC 需要按两字节 NAL 头区分 VPS/SPS/PPS、IDR 与其他图像，把经验证可独立解码的起点交给环形缓冲。使用现有 `au_ring.c` 强制覆盖老数据后，归档 HEVC 的 15–29 帧可以从中途 IDR 独立解码，逐帧与完整流对应区间一致。归档是 QEMU 生成的 1024×682 对照，当前机身强制帧能否周期性得到原生 IDR 待采样。初版低延迟路径需要无 B 帧，现有 `stts` 路径没有重排序所需的 `ctts`。[验证与边界](HEVC-ENCODER.md)
3. **852 行与封装。** 本次把归档 HEVC writer 的 VPS/SPS/PPS、`hvc1/hvcC` 和 SPS 几何校验移植到临时的当前 `src-fix31` 副本，保留现有方向矩阵、Apple 时间轨与音轨 writer，默认入口仍是 AVC。HEVC 选择由实验入口显式传入。若确信源的顶部 852 行有效、底部 12 行仅为填充，受限的 C helper 可在写入 `hvcC` 前修正无裁剪 SPS。夹具覆盖原码流裁剪正确、移除裁剪后默认拒绝、显式确认再修复三条路径。调用条件与拒绝项见编码报告。生产集成仍需处理 VPU 输入槽的 Y/UV 边缘填充和 cache clean。[实验补丁](evidence/hevc-852/writer.patch) · [有声夹具](evidence/hevc-852/probe.c) · [裁剪验证](evidence/hevc-encoder/check_crop.py)
4. **先验证编码器，再换源。** 建议先在已知运行的 1024×682 ch1 获取真实 Main8 的 VPS/SPS/PPS、首个与周期性 IDR、PTS 和完整 GOP，再单独接入已证明有持续 DMA 的 852 行源。两次都要覆盖正常停止、切回放和切录像后的服务归还。切换 codec 不会解除当前 Wave5 的共享资源和交接约束。

## C 3.11 的 HEVC 播放有第二道检查

`0x40ce8a08` 处理 MOV 外层和轨道。在工作区的真实固件中，`0x403457a4`、`0x403458a0` 随后调用 `0x40ce8470` 取 VPS/SPS/PPS，失败各自跳到错误分支。该函数读取 `hvcC` 盒起点 `+9`，即配置记录第二字节（profile space、tier 与 profile ID）；`0x40ce84ec..0x40ce84fc` 仅接受 `0x21` 或 `0x22`。随后还按顺序取三组表，每组要求一个参数集。以 guest 内存代理向真实机器码提供文件数据，实跑如下。[复演入口](evidence/hevc-852/verify.py) · [ARM64 夹具](evidence/ncdt-replay.S)

| 合成文件 | 打开 MOV | 提取 HEVC 参数集 |
|---|---:|---|
| 完整 Apple 写法，保留 `mebx`；只加 `niko`；再去 `mebx` 但音频仍是 `co64` | 三件均为 6 | 均未进入 |
| `niko`、无 `mebx`、音视频均为 `stco`，Main profile / Main tier（`0x01`） | 0 | **4** |
| 仅把上一件 `hvcC[1]` 改为 `0x21`，VPS/SPS 仍为 Main tier | 0 | 0，能取出三组。这个故意不一致的文件仅在内存中构造，用于定位检查条件。 |
| 由同一码流构造 High tier 对照，在 VPS、SPS 的 general PTL 同步声明 High tier，让 writer 由 SPS 生成 `hvcC[1]=0x21` | 0 | **0**，依次得到 VPS 24 B、SPS 42 B、PPS 7 B，首个 NAL 类型分别为 32、33、34。 |

两件相机封装样片的 `mdat` 逐字节相同。High tier 那份更换的是参数集中的声明，此实验没有改变传感器、编码质量或硬件预算。C 3.11 的 HEVC 参数构造将 `+0x28` 设为 1，既有两份实机 4K/8-bit 录像参数也实读为 1，指令把这个选择写入序列寄存器 `+0x124` bit 12。照片档的实现应使用真实返回的 VPS/SPS 推导 `hvcC`，核对 tier 和 level。单独伪改 `hvcC` 只能越过这项头部检查。两件分发的测试文件都没有 `NCDT`；此前的 metadata/DCF/机身解码和退出仍是独立门槛。[硬件与配置证据](HEVC-ENCODER.md) · [NCDT 实验](FIRMWARE-NCDT.md)

## 样片与复现

本次用本机 VideoToolbox 对 1280×852 动态测试图请求 30000/1001、Main8、12 Mbps、禁用 B 帧。观察到 6 张 I、84 张 P，关键帧索引 0、16、32、48、64、80，单 AU 最大 74,908 B。90 帧 Annex B 共 4,538,816 B，比 5 MiB 压缩环容量小 704,064 B；其它内容的码率峰值、IDR 间隔以及真机持续帧率需要另测。两个 MOV 均是 5,117,631 B，时长 3.003 秒，SPS 显示 1280×852（编码画幅 1280×864，逐行 4:2:0 的底部 conformance offset 为 6），音频 144,144 个双声道 PCM 采样帧。裸码流、Apple 封装和相机封装独立解出的可见像素一致。

| 校验项 | SHA-256 |
|---|---|
| Main tier MOV | `4e2ec9a38b755ea1b60d700390b162dcc215b33c541a6514da79592181e80105` |
| High tier MOV | `92e1e4f90920ee63ce533b6aa748c6aeb572006ec8b866ebbba728f1ad131260` |
| 解码画面，全部 90 帧 | `fb742ebf2b17ff07d19fe26c2e7263ee9fa26396578b647f63a6fbcf46fb6d59` |
| 解码 PCM | `6f40fd12b6deeb85ef7e996531855ba9a3a82e23e729d54a0c75bf83b6eb3877` |

在 `feat-mov` 运行。可以从已交付的 Main-tier 文件恢复当前测试码流，然后用本机 FFmpeg、ffprobe、clang、patch、QEMU 和带 pyelftools 的 Python 复验。这样不需再次使用主机硬件编码器。

```sh
work=$(mktemp -d)
ffmpeg -v error -i evidence/hevc-852/camera-1280x852.MOV -map 0:v:0 -c:v copy -bsf:v hevc_mp4toannexb -f hevc "$work/control.hevc"
../feat-rec/.recorder-venv/bin/python -B evidence/hevc-852/verify.py --source "$work/control.hevc"
../feat-rec/.recorder-venv/bin/python -B evidence/hevc-encoder/check_encoder.py
../feat-rec/.recorder-venv/bin/python -B evidence/check_ch2_queue.py
```

复验工具支持 `--ffmpeg`、`--ffprobe` 指定可执行文件。也可在有编码能力的系统上以 `--encoder hevc_videotoolbox` 或 `--encoder libx265` 从测试图重新编码；编码器版本、平台和生成的参数集会影响文件哈希。要只检查文件，可运行 `python3 -B mov_fix.py evidence/hevc-852/high-tier-1280x852.MOV --inspect`。输出同时列出 hvcC、SPS 和 VPS 的实际 profile/tier 字节与 level，以及是否满足已验证的固件单字节条件。上述主机验证没有启动完整 Z8、连接 USB 或制作升级包。
