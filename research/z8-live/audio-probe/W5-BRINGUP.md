# Wave5 在拍照模式下的 bring-up 布局（C3.11）

依据：stock MAIN 静态反汇编（sha `2b43a31c…`），加 2026-11-05 真机读数（`remote-rec` / `sample` / `heap-test` / `w5-plan`）。早期的全链静态报告见 `_mac-migration/.../codex-tasks/w5-init/REPORT.md`，本文只补充有实测支撑的参数和拍照模式方案。

## 1. 硬件上电入口 `0x402f2d58(W0 client, W1 kbps, X2 arena, W3 size)`

| 参数 | 含义（实证） | Live 取值 |
|---|---|---|
| W0 client | 0–4。录像调用点 `0x40d07ba4` 用 0/1（按 `[x20+0x20]` 选择）；回放模块（MOVS，`0x40cf9810`）用 `[0xa0319b72c]`，plan 表 `0x42a00928 = {2,2,3,0}` | **4**（stock 没有调用点用到） |
| W1 | **码率，单位 kbps**：两个调用点都是 `值 × 0x10624dd3 >> 38`，也就是 ÷1000。上电序列里 ≥240000 选时钟档 8，否则选 9 | 约 8000–30000，落在档 9 |
| X2 arena | 必须满足 `(base & 0xfffffff800000fff) == 0x800000000`，即 0x8xxxxxxxx 范围内、4 KiB 对齐。4K 录像时实测是 0x8c47d1000 | 池分配得到的地址（例如 0x8a83fb000，符合要求） |
| W3 size | 录像和回放都传 0x1000000（16 MiB）。VDI 从中切出 code 2 MiB、TEMP 2 MiB 和 0x201000 | 16 MiB |

上电成功的判据（RAM 读数，不读 MMIO）：VDI `0xa02d3d928 +0x58 = 0x16010000`，`+0x28 = arena`，三个 board handle `0xa02e98488..498` 非零，`microcode_staging +8 = 1`。

退出：`0x402f3084(client)`（回放模块的写法：`0x40cf9864` 读 client 后直接 `b 0x402f3084`）。退出后 VDI 和实例表清零，board handle 保留。

## 2. 内存需求（stock 计算函数实测）

`0x40c341ec(in, out)` 是纯计算（`0x40c3924c`，只读常量，不碰硬件）。in = {codec(1=HEVC,0=AVC), w, h, kbps, gop(0–3), stream(0–2), depth(8/10)}。

| 配置 | 参考帧（压缩） | 源帧 | 码流缓冲 | WORK | MV |
|---|---|---|---|---|---|
| HEVC 3840×2160 120 Mbps gop0 | 2 × 12.1 MiB | 11.9 MiB | 21.5 MiB | 1.74 MiB | 0.5 MiB |
| HEVC 1920×1080 30 Mbps gop0 | 2 × 3.1 MiB | 3.0 MiB | 5.4 MiB | 1.74 MiB | 132 KiB |
| **HEVC 640×432 8 Mbps gop0** | **2 × 448 KiB** | **405 KiB** | **1.43 MiB** | 1.74 MiB | 24 KiB |
| 同上 gop1/2/3 | 3/4/5 个参考帧 | | | | |

（码流缓冲 = kbps×1000/8×1.5，即 1.5 秒；两个常量 0x1000000 和 0x6400000 只出现在 plan 汇总里，4K 实录的 VDI 并没有用到 100 MiB 的那一块。）

**640×432 预算**：16 MiB arena + 1.74 MiB WORK + 约 1 MiB 参考帧及 FBC + 若干源帧（每帧 405 KiB）+ 1.5 MiB 码流，合计约 21 MiB。**向照片池要 2 个单元（21.8 MiB）就够，留余量就要 3 个（32.8 MiB）**。只会少几个连拍缓冲，不碰录像专用的 489 MiB 区。

## 3. 拍照模式下的内存来源（实测）

- 录像布局 G2（`0x44e8e5a0` 表 #50–73）里的 Wave5 区，在照片布局（大概率是 G4）中归连拍缓冲池所有。单张拍摄就会改写其中 260 MiB，所以**不能用固定地址**。
- RTOS 堆只有 10 MiB + 4 MiB，不够。
- 池分配器 `0x4104d2dc(0x4c4155, 0x60, bytes)` 和释放 `0x4104d600(d)`：按 10.9 MiB（0xaec000）为单位分配连续内存，从 0x8a8ee7000 往下长，512 MiB 以内实测都成功。描述符 `d+8 → {cpu, actual}`，类别在 `d+0x10`。

## 4. 建议的独立流程（Live 自己当 client 4）

1. `d = pool_alloc(3 units)`。前 16 MiB 作 arena（4 KiB 对齐），其余切成 WORK、参考帧、源帧环和码流。
2. 确认没有录像或回放占用 Wave5：VDI `+0x58 == 0`，且实例表全零。
3. `0x402f2d58(4, kbps, arena, 0x1000000)`，检查 VDI `+0x58`。
4. CREATE `0x40c57ff4`（cmd 8）→ SET_PARAM（cmd 0x200，640×432、gop0、8-bit）→ SET_FB `0x40c58f54`（cmd 0x80，参考帧和 FBC 来自池）→ header（cmd 0x100）→ 每帧调用 `0x40c59890`（源 YUV 平面地址写入 `+0x148/14c/150`）→ FINISH `0x40c5a43c`（cmd 0x20）。所有命令都经 `0x40c541e0` 串行执行，完成靠 INTID 120 → HEVC 服务通知。
5. `0x402f3084(4)` → `pool_free(d)`。

## 5. 风险和待验证

- 步骤 3 会碰电源、时钟和 reset 域（九个 reset `0x43..4b`）。这些和 still/ISP 共享多少还没有证明。拍照中途上电可能干扰正在进行的照片处理，所以第一次只在**空闲取景、不按快门**时做，出错就重启相机。
- 实例相关的结构（CREATE/SET_PARAM 的参数块）还没有逐字段对上；4K 实录的实例快照（`w5-h265-rec.json`）可以当参照：+0x1f8 是 2160/1001，+0x200 是 30000/1000，+0x1e0 是回调 0x402f63b4。
- 被 power-cycle 后，board handle 会清零（2026-11-05 观察到）。上电函数会自己重新获取。

## 6. 2026-11-05 真机结论与编码接入点

- 拍照取景下 `0x402f2d58(4, 8000, 池地址, 16 MiB)` 上电成功（0.25 s），上电期间单拍/连拍正常；**退出 `0x402f3084` 会让相机卡死**（随后 Fenestra 报 Card error）。Live 只上电、拍照模式下永不退出。
- 照片池分配/释放单独测试安全。

### 编码器适配层（录像用，0x402f55xx–0x402f6xxx）
接口表 0x402f6bd4..6c0c：回调注册 5578（存 0xa02e98df0/df8）、缓冲绑定 5624（type0 码流 / type1 arena+帧区 / type2 8 MB / type3 源帧 / type4 输入别名；AVC 布局 type1 只需 42 MB）、启动 5870（0x1c 参数 → 0xa02e98ed0，事件 1）、提交帧 5a48（0x28 B 描述符入 30 格环 0xa02e98fa8，+0x08/+0x18 为两个平面地址）、停止 58f4 等。任务每攒 4 帧调服务层 0x40c33e34。

### 编码启动处理 0x402f4560（服务层调用顺序）
1. 0x402f3264 模式表 → sp+0x108：+0x4 u16 fps?、+0x6 宽、+0x8 高、+0x18 gop、+0x1c bps、+0x20 u16 mode
2. stream = 0x402f2b94(mode==2) → 0xa02e99ff8
3. 0x40c33834(stream, 回调 0x402f63b4)
4. cfg(0x20 B: codec≤1, w, h 按 32 对齐, kbps, gop≤3, mode, fps, fps) → 0x40c333d0(cfg, out 0x38 B)
5. 缓冲注册表 0xa02e9a000（0xf0 B，取自适配层 0xa02e98e00 绑定状态）→ 0x40c33480(stream, 表)
6. 0x402f6c10(模式参数, gop/?, buf, out 0x220 B) → 0x40c338ac(stream, out, flag)
7. 0x40c33d98(stream, &p, &n, 0xa02ea310) 取码流头
8. 运行中：0x40c33fb0 触发、0x40c33e34(stream, flag, 4×(Y,C)) 提交

### 2026-11-05 真机：拍照模式下建成 H.264 1024×680 编码实例（probe `w5-open`）
- 上电 client 4 → 驱动分配 **stream 0**（映射表 0xa02e98480[client]，0xff = 无）。client→(type,arg) 表 0x42a002a0 {1,3,1,3,1} / 0x42a002c0 {0,2,0,2,0}；退出时 0x402f2bac 置 0xff。
- 顺序：open 0x40c33834(stream, cb) → configure 0x40c333d0 → register 0x40c33480(stream, 0xf0 表) → 0x402f6c10(模式结构, 0, 用户区, out 0x220) → set 0x40c338ac(stream, out, 0) → headers 0x40c33d98(stream, &p, &n, lens[4])。全部 rc 0。
- 码流头（AUD + SPS + PPS）：`00000001 0910 00000001 6764002aacd2010015fe59b808080a000007d20001d4c108 00000001 68ea8f2c` → High@4.2，64×43 MB，裁 8 行 = 1024×680。
- worker 状态数组 0xa02d3274c+worker×4（stream 0 → 4），结果字 0xa02d32770+stream×4；回调表 0xa02d30ad0[stream]。
- 缓冲布局（池块 0xa118a5e98，54.6 MB @0x8a3787000）：frame 20 MB、sub1 512 KB、sub2 5 MB、bs0/bs1 各 2 MB、aux 4 MB、ref0/ref1 各 2 MB、ext0/ext1 各 2 MB、user 512 KB；arena 用上电的 16 MB（0x8a6e23000）。

### 2026-11-05 真机：硬件编码 4 帧成功（probe `w5-enc`）
- B1 四个槽位用原生 memcpy 0x4127a900 拷进窗口内缓冲（Y 1024×680 @+0，C @+0xb0000；池内存和 B1 都是 Normal Non-cacheable，属性 0x44，无需缓存维护）。
- run 0x40c34164(stream, {1,1,15,15}) → submit 0x40c33e34(stream, 0, 4×{u64 Y, u64 C}) → trigger 0x40c33fb0(stream, 0, i, 0, i==0) i=0..3，全部 rc 0。
- bs0 输出：AUD+SPS+PPS+I(14018 B)，然后 3 个 P（10077/12461/20006 B）。ffprobe：h264 High 1024×680。
- 与源帧对比 PSNR：Y 48.2 dB、C 46.4 dB（源 Y 均值 23.9，暗场景）。

### 帧信息结构（0xa02d38b98 + stream×0x2400 + idx×0x480，每 stream 9 格循环）
+0x00 u64 本帧码流地址（第 0 帧跳过 SPS/PPS）、+0x08 字节数（含 AUD）、+0x2c 类型（0=I,1=P）、+0x40 帧序号、+0x60 90 kHz PTS（每帧 +3000）、+0x94 切片字节数。回调 cb(stream, 类型 0..4, info)：类型 0 = 一帧完成（info 指向上面这一格），类型 2/3 为其他事件。
- 触发参数 w4 只能 0/1：1 = 强制 I 帧，但 NAL 仍是 1（非 IDR）；运行中重设参数（+0x64=15）不生效。→ 封装时改写切片头：片段首个 I 帧改为 IDR（类型 5、插入 idr_pic_id、dec_ref_pic_marking 两位），片段内 frame_num/POC 归零重排；PPS 为 CABAC（切片数据字节对齐，只重写头部）。
- 离线样本：build-tools/w5-samples/w5-hw10.h264（10 帧，含 SPS/PPS）与 w5-info10.bin（9 格信息）。

### 2026-11-05 连续编码细节（真机 300/150/160 帧）
- 完成判定：第 n 帧在帧信息环 n%9，+0x60 = n×3000（+0x40 在 GOP 周期 I 帧处归零，不能用）。
- 码流环：bs0/bs1 相邻，VPU 当作一个 4 MB 环。帧到达任一缓冲末尾时会**分段**：末尾留几字节不用，从另一缓冲开头继续；帧信息 +0x0c=1，第一段 {+0x10 地址, +0x18 长度}，第二段 {+0x20 地址, +0x28 长度}，两段之和 = +0x08。
- 硬件也会自己周期性出真 IDR（NAL 5），改写器对其原样保留，只用它作 P 帧重排基准。
- 端到端：150/160 帧 → 改写 → v4 live_mov.c → MOV 1024×680 High，ffmpeg 全解码零错误。

## 7. 与原厂录像/回放共用 Wave5（2026-11-05，静态分析 + 固件改动，真机未验证）

### 原厂行为（C3.11 静态）
- 上电 `0x402f2d58` 每次都会重跑整套时钟、reset 解除、电源序列，然后由服务层 `0x40c32fec(cfg, type, &stream)` 分配 stream：
  - 最多 2 个 stream，slot 表在 `0xa02d30118` / `0x124`，每项 `{used, coreA, coreB}`。
  - 核忙标志：`0xa02d30130`（A）、`0x134`（B）。
  - client→type 表 `0x42a002c0 = {0,2,0,2,0}`：type 0 要核 A，type 2 两个核都要。所以 Live（client 4）、录像（client 0/1）、回放（client 2/3，plan `{2,2,3,0}`）都要用核 A。
  - VDI 已上电时，第二次 open 必须传入相同的 arena 和 size（与 `0xa02d30148/150` 比较），否则返回 4。
- **录像调用点 `0x40d07ba4` 和回放 `0x40cf9810` 都不检查上电返回值**。上电被拒时只走 trace `0x40ca1990`，之后照常用 client map 里的 stream（0xff）继续。
- 退出 `0x402f3084`：先关 `map[client]` 对应的 slot，只有在没有剩余 slot 时才 deinit VDI；然后**无条件**把 reset 0x43–0x4b 置位、关时钟、断电，不管别的 client 还在不在用。原厂在每次录像结束（`0x40d0712c`）和回放结束（`0x40cf9864`）时都会调用它。

### 结论
Live 在拍照模式下一直占着 Wave5，会让之后的录像和视频回放拿不到编码器（真机具体表现未测）。反过来，录像结束时的原厂退出会在 Live 的实例底下把硬件复位，但因为 Live 的 slot 还开着，VDI 不会清零。包 2 原来“VDI 为 0 就重建”的判断在这种情况下不成立。

### 改法（build-w5d，MAIN `38a7b36b`，加 USB 默认开启后为 `b8d3b56f`）
- 在 `0x402f2d58` 入口加 hook（原指令 `sub sp,sp,#0x80` 在 trampoline 里重放）。任何非 4 号 client 要上电时，如果 Live 还持有 Wave5，就在调用者自己的上下文里先等进行中的帧完成（最多 0.6 s），再调用原厂退出 `0x402f3084(4)` 归还 Wave5，然后原上电照常进行。“退出后紧接着上电”和两段原厂录像之间的序列完全相同。
- `alive()` 额外检查服务层 slot 是否还开着。别人持有 Wave5 时，编码返回 `LIVE_ENCODER_BUSY`（-30），丢弃这一帧而不是让 Live 进入 FAILED。等原厂用户退出（VDI 为 0）后，下一帧会在拍照取景里重新上电（拍照取景下上电已在真机验证）。
- 源码：`build-tools/w5/live_w5enc.c`、`build-tools/w5/coexist_patch.py`（runtime.c / hooks.S / live_stream.c / live_codec.h / relive_w5.py），由 `setup_src.sh` 统一应用。

### 真机验证顺序（未刷机就能先验证前提）
1. 拍照模式，拨杆在拍照：`w5-up --no-exit` → `w5-open` → `w5-state`（client 4 持有 Wave5）。
2. 人工拨到录像：`w5-state`（看 `photo_movie` 和 recorder 的值）→ `remote-rec`，记录**不处理时**录像的实际表现。
3. 重建 Live 状态后再拨到录像：`w5-release`（录像上下文里的原厂退出）→ `w5-state` → `remote-rec`，录像应该正常。
4. 回放模式：用同样方法分别测“不处理”和 `w5-release --force` 两种情况下视频回放的表现。
5. 刷包 3 后：开 Live 拍几张 → 拨到录像录一段 → 拨回拍照再拍 Live → 播放 Live 和普通视频，同时用 `w5-state` 观察。

### 7.1 Live 录音与回放/语音备忘（2026-11-05，改动已加入包 3 候选 build-w5e）
- Live 录音沿用原厂声音测试的消息序列，开始后音频服务处于状态 5。原厂声音测试在状态 3/5 时拒绝开始；AudioApplicationService（`0x40e3xxxx`）里另有几处按状态分支（`0x40e326e8` 要求状态为 1，`0x40e34eb0` 在状态 0/4/5 时走另一条分支）。回放和语音备忘在状态 5 下会怎样，静态上没法确定。
- 包 2/包 3 的录音开关原来是 `photo && requested`。进回放时拍照标志 `0xa03bd062c` 仍是 0，所以**回放里 Live 会一直录音**。
- 改法：录音另外要求最近 1.5 s 内有取景帧（`capture.irq_timestamp`），或者正在截取片段（`stream.job != 0`，从按下快门到封装完成）。离开取景约 1.5 s 后录音就停，音频服务回到 1。快门黑屏和按下后的长曝光不受影响。
- 真机检查（刷包 3 后）：开 Live → 进回放，约 2 s 后用 `w5-state` 看 audio 是否为 1 → 播放一段带声音的视频，确认有声音 → 给照片录一段语音备忘 → 回到拍照，确认 Live 的片段仍然有声音。
- 刷机前用探针做基线：`audiodbg 1 1 0`（模拟 Live 录音，状态变为 5）→ 进回放播放带声音的视频，记下有没有声音、有没有报错 → `audiodbg 1 0 0`。
