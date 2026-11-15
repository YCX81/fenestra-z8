# V60 回放识别缓存：离线复现

2026-11-15。没有接触相机、USB 或存储卡，没有修改生产补丁、生产源文件或交付包。

结论：已在 V60 的真实 `lookup`、`worker`、`pb_key_notify` 控制流上复现一个旧的时序缺陷：第一次识别照片时 MAN 文件尚不可读，之后 MAN 已经可读，只要仍停留同一张照片，worker 就不会再执行识别；Live 标志不会被请求绘制，REC 也不会被 Live 接管。**这证明代码路径存在，不证明用户这次相机已经走到该路径。**

## 来源与执行方法

`generated/` 只复制 `feat-v53/integration-g-final/work` 中补丁实际修改的八个文件，再执行当前 `patch_om7.py` 链，未设置 `OM_FORCE` / `OM_NORULE`。生成的 `pb-fix37/runtime.c` SHA-256：

`0440094422da6f25420e0cd6d242b86b76b674799d90241bd6d74a63fc8ab2d3`

该值与主任务核对的 V60 `om8src` playback runtime 相符。V60 的帧数规则没有修改 playback runtime；这个缺陷不能归因为 V60 新增的减帧代码。

从 Nikon 工作区重跑：

```sh
python3 build-tools/livediag/feat-onemov/v60-diagnosis/cache-repro/run_repro.py
```

脚本先校验上述 runtime hash，再原样提取 `pb_close_gate`、锁、`hide`、`ready_to_write`、`present`、`nb_end`、`lookup`、整个 `worker`、`pb_system_notify`、`pb_key_notify`。使用真实 `PbRuntime` 类型及原始 `sync.c` 的路径与 MAN 解析。编译启用 AddressSanitizer / UndefinedBehaviorSanitizer。源片段行号、逐段 hash、编译命令和输出在 `results.json`。

替身边界：原厂文件 IO、页面/当前照片查询、时钟、调度、同步 GC 和 GUI 后端被明确 stub；MOV 尺寸 probe 也被 stub 为成功。worker 的循环、分支和上述函数没有改写。`badge_requests` 是请求生成徽标的次数；`rec_owned` 是真实键处理返回的接管结果，二者都不代表实际显示或解码已经发生。

## 结果

八项检查通过，sanitizer 无输出：

| 条件 | 结果 |
|---|---|
| 首次 MAN 失败，随后变可读；同照片等待 4 秒 | lookup/读 MAN 均只有 1 次；cache_valid=0；无徽标请求，REC 不接管 |
| 同上，切到另一张再切回 | lookup=3，cache_valid=1；徽标请求与 REC 接管恢复 |
| 同上，退出回放再进入，worker 观察到一次非回放态 | lookup=2，cache_valid=1；徽标请求与 REC 接管恢复 |
| 正常拍摄态进入回放，worker 观察到一次非回放态 | ui_leaving 被清除，识别/徽标请求/REC 接管正常 |
| ui_leaving 单独卡在 1，MAN 已就绪 | 徽标请求被阻止，但 REC 仍被接管；它单独不能解释两项症状 |
| 普通键使 key_epoch 改变，然后静置 | blocked_key_epoch 在超过 700 ms 后清除，徽标请求和 REC 接管恢复 |
| 媒体事件 24 清除有效缓存，仍显示相同照片 ID | lookup 仍只有 1 次，cache_valid=0；不会自动重查，REC 不接管 |
| bridge stage 3 且 ui_leaving=1 | 超过 2.5 秒后清 stage/ui_leaving，徽标请求和 REC 接管恢复 |

## 代码位置

以下行号均是 hash 已核对的 `generated/pb-fix37/runtime.c`：

- 285–295：`lookup` 先清缓存；MAN 读取/解析、native 路径或原图大小校验失败都直接返回。
- 333–344：只有 `id != lastid` 才调用 `lookup`；正常停留同一照片不会重试。`!active` 会把 `lastid` 清零，因此退出再进入能触发重新识别。
- 396–402：REC 接管明确要求 `cache_valid`；没有 `ui_leaving` 条件。
- 378–387：媒体事件会将 `cache_valid` 清零，但 worker 的 `lastid` 并未随之失效。
- 348–356：V58 继承来的桥接关闭保护独立控制 `ui_leaving`，stage 3 为 2.5 秒。

MAN 的 writer 最后才创建并关闭该文件，见 `generated/src-fix37/codec/live_transaction.c` 的 `create_exclusive(...,manifest,...)` 段。因此“第一次回放识别早于 MAN 完成”是合理的触发条件，但本次没有相机新拍文件、日志或真实调度证据。

## 最小修复方向（未实施）

在 worker 对当前照片的无效缓存增加限频重试，并在媒体 generation 变化后允许同 ID 重新识别。保留现有 MAN 解析、完整 native 路径和原图大小身份校验；不要把缺 MAN 自动当作有效 Live，也不要在 UI/键回调中读卡。重试应仅在稳定的照片回放态、`nb_stage == 0` 且 `ui_leaving == 0` 时执行，避免在原厂短片播放及退出保护期间插入文件读取。应为普通非 Live 照片使用有限频率或退避，避免每个 5 ms worker tick 都读文件。

真实故障仍需核对：新拍同名 JPG/MOV/MAN 是否存在且一致、切图/退出或重启后是否恢复。若 MAN 根本没有写出，修缓存重试不能修复上游捕获/写卡失败。

## 同日追加：实际卡文件与重启反馈

用户随后确认旧 Live 有标志，9085–9090 无标志，重启仍然无。主任务只读复制了卡证据到 `../card-20261115/`。这一反馈使“只在首次进入回放时 MAN 迟到”不足以解释完整故障，不能据此直接发布缓存重试修复。

实际清单中，9085、9089、9090 有完整的 LIVE JPG/MOV 和 MAN，MAN 的 native 为 `B:\DCIM\108NCZ_8\DSC_xxxx.JPG`，所记大小与 DCIM JPG 大小一致；9086–9088 没有对应 Live 文件。旧 9084 的 native 同样为 B:。新完整三组的 `volume_generation` 为 3、3、2，旧 9084 为 1。

`check_card_manifests.py` 将四个实际 MAN 原始字节和清单原图大小送入上述 hash 已核对的**原样 `lookup` 和真实 `sync.c`**。20 项输入检查全部通过，结果在 `card-lookup-results.json`：

- 对 9084/9085/9089/9090 分别提供正确的 B: JPG 路径，`media_epoch` 无论 1 或 3，均得到 `cache_valid=1, lookup_error=0`。
- 分别提供错误 A: 路径、NEF 后缀或错误原图大小，均得到 `cache_valid=0, lookup_error=2`。
- 因此 MAN 的格式、UUID、大小和 generation 数值本身没有复现拒绝。此版本 `lookup` 已不比较 MAN generation 与运行时 media_epoch；不能把新文件的 generation=2/3 当作原因。

该测试仍以替身提供原厂路径和 IO 返回值，尚不知道相机上 `407b95e4(current_id, native_path)` 实际返回了什么。`current()` 将 `407b4e78()` 给出的 ID 原样传入；lookup 没有改低四位格式、没有尝试 JPG sibling、没有跨卡寻找 MAN。

清单另有差异：旧 9084 在 B 卡同时有 NEF/JPG，新六张只有 JPG。源码中 writer 只配对 JPG ticket（生成的 `src-fix37/runtime.c` 中 `live_try_encode`），Live 归属 JPG 所在卡；`live_twin_step` 只补写另卡同名 JPG，不为另卡的 NEF 创建 Live。若当前是 RAW 主卡/JPEG 副卡布局、回放选中了只有 NEF 的主卡，就可能找不到 B 卡上的 Live。**这只是与证据相容的候选，不是已经确认的相机设置或根因。**

下一条最有区分力的证据是 9084 与 9085/9089/9090 回放时的实际 `current_id`、`native_path`、`lookup_error` 和读文件返回值。若正确 B: JPG 路径及读大小都成立而仍拒绝，则应核查运行中固件/最终机器码判定与源文件的一致性，而非继续修改 MAN 数据。
