# V60 实际 MAN 与回放路径检查

2026-11-15。仅使用本地只读复制的卡证据及源文件；未更改生产补丁、固件包或卡。

**结论：9085、9089、9090 的 MAN 本身满足 V60 的识别条件，generation=2/3 不会导致该版本拒绝。尚未确认本次无标志的根因。** 重启后仍无标志，说明先前复现的“同 ID 缓存失败不重试”不能独立解释完整故障。

## 文件证据与真实代码检查

依据 `card-20261115/inventory.json`，9085、9089、9090 的 LIVE JPG/MOV/MAN 均存在，MAN 的 `native=B:\DCIM\108NCZ_8\DSC_xxxx.JPG` 与 DCIM JPG 大小一致。9086–9088 没有对应 Live 文件。旧 9084 也使用 B: native 路径。

运行：

```sh
python3 build-tools/livediag/feat-onemov/v60-diagnosis/cache-repro/check_card_manifests.py
```

测试将 9084/9085/9089/9090 的实际 MAN 字节及清单中的原图大小送入 V60 原样 `lookup`、真实 `sync.c`。runtime SHA-256 为 `0440094422da6f25420e0cd6d242b86b76b674799d90241bd6d74a63fc8ab2d3`。共 20 项检查通过：

| 提供给原厂路径/文件 IO 替身的条件 | 四个 MAN 的结果 |
|---|---|
| 正确 B: JPG 路径、正确大小、media_epoch=1 | 全部 cache_valid=1，lookup_error=0 |
| 正确 B: JPG 路径、正确大小、media_epoch=3 | 全部 cache_valid=1，lookup_error=0 |
| 改为 A: 路径 | 全部 cache_valid=0，lookup_error=2 |
| 改为 NEF 后缀 | 全部 cache_valid=0，lookup_error=2 |
| 原图大小与 MAN 不同 | 全部 cache_valid=0，lookup_error=2 |

结果：`cache-repro/card-lookup-results.json`。测试使用真实 MAN 和识别代码，但**原厂实际返回路径、IO 状态和大小由替身提供**，不是相机运行时观测。

## 确定的能力边界

`cache-repro/generated/pb-fix37/runtime.c:66` 的 `current()` 读取 `407b4e78()` 提供的原厂当前照片 ID；285–294 行把该 ID 原样交给 `407b95e4()` 得到路径，再检查该路径对应的 MAN。没有改低四位格式、没有尝试同组 JPG、没有跨 A/B 卡寻找配对文件。此处已取消 MAN volume_generation 与 media_epoch 的相等检查，仅由 parser 要求 generation 非零。

Live writer 只配对 JPG ticket；Live 文件写在该 JPG 所在卡。`live_twin_step` 只对另卡同名 JPG 补写，不为另卡只有 NEF 的情况创建 Live。因此 **RAW 主卡 + JPEG 副卡时，回放主卡 NEF 不具备跨卡定位副卡 JPG/Live 的支持**。这是源码确定的边界，当前用户是否使用这一双卡布局仍待确认。

卡证据中旧 9084 在 B 卡有 NEF+JPG，新六张在 B 卡只有 JPG。这与上述双卡候选相容，但也可能有其他设置或文件布局原因，不能据此认定用户改了双卡设置。

## 尚缺的区分证据

比较旧 9084 与新 9085/9089/9090 回放时的实际 `current_id`、`native_path`、`lookup_error`、文件打开/读取返回值及原图大小，可区分跨卡/格式定位和 IO 问题。优先确认回放正在显示哪张卡、是否正在显示 NEF。文件证据不足以推断原厂 API 当时的实际返回值，本轮不发布生产修复。
