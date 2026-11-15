# CPU 编码／MOV／JPEG 配对实现及边界

本目录实现可独立编译的 C 后端，**未调用 QEMU**。`live_codec.h` 是 I420 → Annex-B AU 的小接口，`live_transaction.h` 是上层文件事务接口；全部内存由调用者一次分配，码流在下一帧编码前有效。上层须在可抢占 CPU2 任务调用，不得持原生锁跨编码。

- `minih264e.h` 原样取自 `https://github.com/lieff/minih264/blob/master/minih264e.h`；SHA-256 `6df33ce8402b41320c5f75a1eab4197fe48130f58829aecd4d67a53b19a91fa9`，上游 public-domain/CC0 声明保留。shell DNS 不可用，通过 GitHub connector 只读取得。单线程、无 SVC、无去噪、`const_input_flag=1`、fastest、600000 B VBV 令上游选择 Baseline Level 3.0；限定 640×424、输入 plane/stride 16 B 对齐。AArch64 使用 NEON，原生上下文保存仍需设备验证。
- `live_mov.c` 逐 AU Annex-B → 4 B NAL 长度前缀，SPS/PPS → avcC，首帧要求 IDR；不伪造固定帧率。使用每帧输入微秒 PTS 生成 stts、相邻真实间隔、最后一帧显式时长；拒绝非递增 PTS。UUID 写入 moov/meta UTF-8，mebx 的 key 为 `com.apple.quicktime.still-image-time`、dtype65、样本 `0000000900000001ff`；edit list 定位到覆盖 press 时间的实际帧。没有音轨。
- `live_jpeg.c` 仅在**副本**原有 EXIF MakerNote 空间写 Apple 0x0011 及更新原条目 count；不移动 JPEG/MPF 字节，不重建 EXIF，不影响原件；副本不再保留 Nikon MakerNote。原 note 不足／缺失则失败，避免破坏 MPO。
- `live_transaction.c` 强制原生 writer 已 close/registered 的逐张 ticket。由该路径 `DSC_####` 派生 `LIVE####`，从不分配 DCF 编号。**create_exclusive 回调必须有原子拒绝既存文件语义，不能“查存在→创建”冒充排他创建。** 后续静态复核已证明 C3.11 `0x402daa4c(...,0x101,0x20)` 经同一互斥 wrapper 对既存文件返回0x80000002，详见 `FS-EXCLUSIVE.md`；运行碰撞测试尚未做。 所有 seek 验证实际位置，每次 read/write 验证实际字节，每个 close 检查结果。失败只删除本次成功创建的文件；删除失败返回 orphan 位。每个 JPEG 块、编码帧之间取消/让出；一次编码期间取消最长等待一帧。NEF-only 仅 MOV。

`bash codec/build_and_test.sh` 可复现：

- host 90 帧**变化的合成场景** → H.264 Baseline Level30 / avc1 / 640×424 / 90 帧；FFmpeg 全部解码，MOV 212018 B。时间轴 3.033303 s，其中一个真实双间隔 66666 μs；mebx PTS=1499985 μs。该素材不是来自新 QEMU LV，不作为设备功能验收。
- 用旧 `live-probe-20261104/run07/DSC_0001.JPG` 做副本：ExifTool 读出与 MOV 相同 UUID。文件长度同为1007328 B；Pillow 完整解码 MPO 三幅 8256×5504、640×424、1620×1080，所有像素与原件逐字节相同。见 `artifact-results.json`。
- 事务注入覆盖34个 I/O 失败位置、14个短传输位置、14个 seek 错误位置；通过既存 JPG/MOV 不覆盖、取消回滚、清理失败报告、未关闭 ticket 拒绝、NEF-only 与源文件未改测试。ASan 检测 vendor+本地代码；UBSan 检测本地代码。上游刻意将 NULL 加小偏移并以 IS_NULL 判断，完整 vendor UBSan 会报 pointer-overflow，因此 vendor 不加 UBSan；没有悄悄修改上游。
- `clang --target=aarch64-none-elf -Os -ffreestanding -fno-builtin` 编译全部成功，四个 object text 合计43278 B，无 data/BSS；未定义符号仅 memcpy/memmove/memset/memcmp。native 链接应提供这些普通内存函数。`-D__linux__` 仅选中 minih264 endian 分支，由本目录 freestanding/endian.h 提供常量，不调用 Linux API。
- H264E_sizeof：persistent936560 B、scratch647167 B；90帧 ring36633600 B；MOV context18832 B，总38236159 B，另加 transaction I/O65540 B、encoder struct、对齐、私有栈。建议外层完整预留后再开始取帧；不能只申请 ring 大小。

生成 JPG/MOV、host 二进制和 AArch64 objects 均在 `/path/to/user/.local/share/nikon-z8-qemu/fenestra-z8/bundle/live-a/host-tests/`。本目录只留源、复现脚本及 JSON/txt 收据。未证：CPU2/NEON 原生运行、原生排他创建的运行碰撞验收、native ticket 接线、同卡并发 I/O、iPhone 指定导入路径、硬件编码时间。文件服务接口是显式集成边界，不能因 host 成功即声称 PHOTO 拍摄端到端完成。

后续补充：`native_files.c/h` 提供一致 0x402dd 系列的 `live_file_ops` 实际地址适配，0x101 排他创建依据见 `FS-EXCLUSIVE.md`，seek 原生非零成功转为 callback 0 成功，read/write 使用32位完成数、write第五参数NULL，检查 seek/transfer 整数范围。Caller 保有取消与 yield 回调和 context。`native_adapter_test.py` 在 Unicorn 中执行真实 AArch64 adapter，仅替换 native入口为 ABI断言stub；15次调用的参数、返回、计数宽度、错误传播和取消/yield context 均通过。此测试**不等于运行了原生文件系统**。该 adapter 尚未自动接入父目录固件 build。
