# V61：RAW 优先回放时播放对应 Live

已交付 [V61 测试包](/path/to/Nikon/delivery/Z8-v61-Live-RAWPeer-db0c847b.zip)。固件 SHA256：`9c9654c694d61a8c4b289e49ededdc4048a32155e34e710d7e3637702c8f767d`。独立解包确认包内 MAIN 等于最终验证镜像、CRC 正确、非 EG 模块未变；全部文件摘要和 ZIP 完整性通过。交付收据：[final/package.json](final/package.json)。

2026-11-15 用户更新 V61 后回复：RAW 优先回放下“已恢复标志，播放和返回都正常”。这为上述具体行为增加用户实机反馈；先前各离线收据保留其运行时的验证范围，不据此扩展为所有双卡场景均已通过。原始反馈记录：[final/user-feedback-20261115.json](final/user-feedback-20261115.json)。

## 本次改动

用户使用双卡 RAW/JPEG 分存，回放显示 C/X 上的 RAW，而已核实的 Live 视频及 MAN 在 SD 上。V60 仅按当前照片路径找 MAN，因而 RAW 页面没有 Live 标志，也不接管 REC。9085、9089、9090 的 SD 侧文件已完整保存；9086–9088 没有独立 Live 视频，升级无法补出这些片段。

V61 保持原 RAW 媒体 ID 和原卡回放选择，查到有效配对后显示 Live 标志。按 REC 时从对应 JPEG 所在卡读取 Live MOV，退出时恢复原 RAW ID。查找顺序是当前照片、同一原厂 DCF 组的精确 JPEG 成员、原厂“跳至其他卡副本”查询所得 JPEG。跨卡关系采用原厂副本语义，并验证 MAN 路径、JPEG 实际大小以及 MOV 信息，不按同名文件盲配。

首次查询未见完整 MAN 时，每秒重试一次；媒体变化立即使缓存失效。缓存发布与播放启动有非阻塞互斥，并检查当前 ID 和缓存 ID 一致，避免切图瞬间播到上一张片段。播放及延迟关闭期间不重新写入会话的目标路径和视频信息。

临时 MOV 成员位只在原组没有真实 MOV 时添加，结束时仅清理本会话添加的位。源卡与副卡按原厂 22–31 生命周期事件分别记录代次：副卡变化仍可清理源 RAW 的临时位；源卡换代则不清理新索引。媒体事件先发布全局失效，再发布对应卡的代次。跨路径 RAW 配对只用于播放，RAW 删除/保护不会借此同步操作另一条路径的 Live 文件。

## 版本与范围

- 构建入口：[patch_om8.py](../patch_om8.py)，新增查找实现：[lookup.cfrag](lookup.cfrag)。从未强制测试参数的 V60 补丁链生成。
- 与生成的 V60 基线相比，仅 `pb-fix37/runtime.c`、`runtime.h`、`ui.c` 三个源文件变化。移除 V54 后无调用的旧预览渲染函数以容纳新逻辑。
- 保留 V60 的拍摄、写入和帧数规避规则；本次不修复存量不兼容片段，也不补录未产生的视频。
- 最终 runtime SHA256：`2e09c019fa19fbf46da71100b222f4a33b1c10e368157b636290510508ef7dbc`。

## 已验证与边界

真实源码提取的宿主测试共 **92 组通过**，ASan/UBSan 无报告，源码哈希和发布顺序检查通过。覆盖普通 JPG、A/B 双向 RAW 配对、同卡精确 JPEG 成员、MAN/大小/MOV 错误、缓存竞争、引用释放、原 RAW 返回、真实 MOV 位保护、快速按键、重试，以及 22–31 媒体事件。结果见 [tests/results.json](tests/results.json)。

最终 MAIN SHA256 为 `db0c847b333f0a6e07222f2207435e9bf340ce1d544704f6d801ec3ecc1088e5`，PB 使用 36,776 字节，剩余 120 字节。直接执行该镜像中 ARM 桥接代码的 **12 项测试通过**，包括真实 REC 按键入口、原 JPG、RAW→B 卡目标重定向与返回，以及媒体代次和既有 MOV 位保护；见 [final/aarch64-bridge-with-key.json](final/aarch64-bridge-with-key.json)。

普通 JPEG 的最终全系统 QEMU 40 秒回归通过：235 项原生检查、13 项桥接检查全部通过，91 次 picture 解码事件，完整进入视频、返回原照片并清理；见 [final/qemu-jpeg.json](final/qemu-jpeg.json)。初次 44 秒运行已完成同一播放闭环，但在末段待机写入未建模寄存器 `0x0d154340` 时失败，保留在 [final/qemu-jpeg-44-boundary.json](final/qemu-jpeg-44-boundary.json)；没有扩宽 MMIO 容忍范围。

真实 NEF 的单卡 QEMU 40 秒回归同样通过：235 项原生检查、18 项桥接检查全部通过。实际选中路径为 `.NEF`，原厂 ID `0x03200022 → 0x03200025 → 0x03200022`；DCF 成员位实读为 `0x24 → 0x24 → 0x04`，关闭后 RAW 成员保留、临时 MOV 位撤销。使用历史真实 NEF 加合成的直接 NEF MAN，因此证明 RAW 原生记录/播放/返回链可用，不是跨卡副本查询的全系统证明；见 [tests/qemu-raw/raw-only/summary.json](tests/qemu-raw/raw-only/summary.json)。同卡 NEF+JPG 对照实际选中的主成员是 JPG，已单独保留，未冒充 RAW 通过。

宿主与 ARM 隔离测试的原厂服务是显式桩，并不等同于真实双卡验证。现有 QEMU 仅提供单张 NVMe 卡，不能将单卡通过说成物理双卡通过；相机双卡、插拔与底层索引重建时序尚未实测。本次没有连接相机 USB、刷机或写实体存储卡。

同步边界：本次对稳定 RAW 缓存拒绝跨路径 delete/protect 入队；旧同步包装器的 manifest 快照仍未与 worker 查询共用互斥。独立复核发现，若有效性判断与复制间发生完整换片，理论上可复制另一张的 MAN。下游会按目标自己的 MAN、原片存在性和保护状态校验，并不直接传播源照片的保护参数；但已有不一致的目标 sidecar 仍可能被同步。尚未证明真实原厂线程能形成该交错，本次未扩大到旧同步模块重构，也不据此声称所有同步并发已获证明。

原厂接口与媒体事件证据见 [pairing-design.md](pairing-design.md)；桥接设计取舍见 [bridge-design.md](bridge-design.md)。
