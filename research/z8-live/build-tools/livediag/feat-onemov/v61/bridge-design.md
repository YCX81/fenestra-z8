# V61 跨卡 RAW → Live：桥接独立审计

状态：设计与边界审计，未修改生产补丁、`v61/work` 或原卡。审计基线是 `v60-diagnosis/cache-repro/generated/pb-fix37/runtime.c` 的有效 V60 源码，并对照 `patch_e3.py`、`patch_om5.py`、`patch_om6.py`。下列“应”是 V61 实现约束，不表示已验证真机。

## 选定方案：原 RAW 继续作为 DCF 锚点

例：界面当前为 `A:\DCIM\108NCZ_8\DSC_9089.NEF`，已验证配对的 Live 属于 `B:\DCIM\108NCZ_8\DSC_9089.JPG`。保留 A 的当前照片和 DCF 分组，只在桥接期间把该分组的虚拟 MOV 路径重定向到 B 的 Live MOV。**不把 4910 切到 B 的 JPG/MOV ID，也不为播放建立跨卡删除/保护关系。**

| 字段/对象 | 应保存的身份 |
|---|---|
| `current_id`、`cached_id` | 原 A RAW 的完整媒体 ID，包括原格式低四位。徽标和按键归属始终以它为准 |
| `native_path` | 原 A NEF 路径，由 `407b95e4(current_id)` 得到；不能被 B JPG 路径覆盖 |
| `manifest.native_path` | 已校验的 B JPG 路径；manifest 的 `native_size` 应与此 B JPG 比较，不能拿 A NEF 大小比较 |
| `directory` | 若作为 sidecar 目录缓存，应明确派生自目标 manifest；不应再用它推断当前照片身份 |
| `nb_photo` | 启动时原 A RAW 的完整 ID；返回时原样写回 4910 |
| `nb_movie` | `(nb_photo & ~15u) | 5u`，仍为 A 卡、相同 DCF 分组 |
| `nb_from` | 从原 `native_path` 派生 `A:\DCIM\108NCZ_8\DSC_9089.MOV` |
| `nb_to` | 从已验证 `manifest.native_path` 派生 `B:\LIVE\108NCZ_8\DSC_9089.MOV` |
| DCF `nb_node`/bit 5 | A RAW 所在分组的节点，绝不能按目标 B JPG ID 清位 |
| MOV 宽高/音视频事实 | B Live MOV 的事实；桥接会话期间冻结，不得被后续 lookup 改写 |

因此无需新的 B 目标媒体 ID，也无需拆分 `nb_photo` 为 B 节点 ID 和 A 返回 ID。若改成 B 节点方案，必须另外保存 `nb_return_id=A RAW` 并验证播放卡切换语义，超出当前最小设计。

## 最小代码变更位置

1. 配对解析保持 `cached_id=id`、`native_path=所选原厂路径`，把经过身份验证的目标 MAN 放进 `manifest`。候选路径/相同文件名本身不构成跨卡身份验证。匹配细节由 pairing 模块负责，桥接只消费已验证结果。
2. `mov_size()` 当前 V60 第 256 行使用 `native_path` 派生 MOV；改为 `manifest.native_path`。发布 `cache_valid=1` 应在完整配对及 MOV 事实校验成功之后，失败时不能留下上一照片的可播放事实。
3. `nb_start()` 当前第 176–178 行：`to` 改由 `manifest.native_path` 派生；`nb_from` 仍由 `native_path` 派生。启动检查增加 `cached_id == id`、有效媒体 epoch 与可用 MOV 事实。`nb_photo=id` 和 `nb_movie=(id&~15)|5` 保留。
4. `nb_mask()` 仍操作原 A 节点。`open/stat` 仍只匹配 `nb_from` 的完整 30 字节（包括 NUL）；命中后返回冻结的 `nb_to`。它不是“所有 A 卡 MOV 都重定向到 B”，其他路径必须原样交给原厂函数。
5. `pb_info_record()` 仍返回副本，不能修改原厂 DCF 记录；宽高读取冻结的目标 MOV 事实，帧率/声音字段沿用已验证 V60 桥接设置。
6. `pb_native_return(kind=3,page=0x552)` 保留 V60 的 stage 2 → 3 和延迟清理顺序，再把原始 `nb_photo` 写回 4910。不要从目标 manifest 反推返回 ID，更不能把返回值固定成 JPG。
7. `pb_delete_wrap()` 和 `pb_protect_wrap()` 的 sidecar 同步条件增加：`manifest.native_path` 与 `native_path` 完整相等。跨路径 RAW → JPG 映射只提供播放，不把 B manifest enqueue 到 A 的删除/保护动作。原厂 delete/protect 仍收到原对象和原参数，原厂返回码保持不变。

直接 JPG → 同路径 MAN 的旧同步行为继续保留；同卡 NEF → JPG 也属于路径不同，最小版本同样不传播 sidecar 操作。以后如要扩大同步语义，应另行定义组级所有权，不能顺带以“同号照片”替代。

## DCF bit 所有权必须修正

V60 第 181 行将 `nb_bit=m!=0` 当作所有权，第 161 行因而会无条件清 bit 5；这不能区分原厂已有 MOV 成员与本桥接临时新增成员。跨卡工作应趁本次明确该语义：

- 在既有 DCF 锁 + slot 锁下读取原位，只有原来没有 bit 5 才记录 `nb_bit=1` 并置位；原来已有时 `nb_bit=0`，退出不清。
- 清理只清“本会话新增”的 bit 5，不能恢复整个旧 u16，否则会覆盖其他格式成员的合法变化。
- 保存本次 `media_epoch` 和解析出的节点身份。清理时重新按 ID 查节点，先比较 epoch 与节点身份，不能直接解引用旧 `nb_node` 指针；媒体已经变化时不准清新卡/新索引的同号节点。
- helper 的节点范围检查应覆盖完整 `node+0x24` 的 u16，而不只是 `node < slot+limit`。
- stage 3 同一照片马上重播时，不能因“bit 已经为 1”把仍属于旧会话的 ownership 丢掉。最简单策略是仅在 stage 0 接受新桥接，保持 2.5 秒关闭窗口；若保留 stage 3 重播，则必须在同一锁内明确转移旧 ownership，并禁止覆盖仍被旧拆除流程使用的重定向/信息快照。

媒体 epoch 改变后跳过新节点清位可避免破坏新媒体，但不能凭此宣称旧索引的所有临时位都已回收；如果原厂保留旧索引，需要在媒体拆除前另有安全清理时机。当前没有证据支持在 UI 媒体事件钩子里新增阻塞 DCF 锁。

## 会话快照与并发边界

V60 `mov_size()` 会改 `nb_w/nb_h`，worker 切换照片会执行 lookup。V61 应避免在桥接过程中由 B/A 新 lookup 重写正在使用的目标事实：可新增会话专用宽高并在 `nb_start()` 复制，或在 stage 2/3 完全禁止重建 lookup 且在清理后强制重新查询。前者改动更局部。

`nb_from/nb_to`、返回 ID、MOV ID、会话宽高、epoch 必须先完整准备，再以 release 发布 stage=2。消费钩子读取 stage 后使用同一会话快照；媒体 epoch 不同即不再重定向。缓存本身需有可验证的发布代次，避免 UI 在 worker 更新一半时混用 A 照片 ID 与另一张 B manifest。不要把这个要求误实现成 UI 任务做文件 I/O。

V60 `pb_info_record()` 在整个 stage>=2 期间覆盖进入该调用点的 ref，没有精确比较 ref 身份。仅保持原 A 分组不会新增 B 的引用切换，但仍要测试“其他原厂 MOV 在关闭窗口进入”的情况；如需缩窄覆盖，应先取得可验证的 ref→媒体 ID 关系，不能猜 ref 的字段偏移。

## 验证清单

### 宿主单元/桩测试

- A NEF → B JPG/MAN/MOV：`current_id/cached_id/nb_photo` 全部保持原 RAW ID；4910 只经历 `A RAW → A fmt5 → 原 A RAW`；A.MOV open/stat 唯一重定向到 B LIVE.MOV。
- 同卡 NEF → JPG、原 JPG 同卡直连、A/B 方向对换：原 JPG 旧行为不回归；所有返回保留原格式低四位。
- 同目录同编号但身份不同、目标卡未挂载、MAN 不完整、size 不匹配、MOV 事实失败：不显示可播放缓存、不消费 REC、不置 DCF 位、不切页。
- REC 排队后当前照片/媒体 epoch 改变：拒绝旧请求；不沿用旧 target 或旧宽高。
- DCF bit 5 初始 0/1：结束分别恢复 0/保持 1；在播放期间其他格式位改变后仍保留；节点查找失败不发布 stage 2。
- stage 3 重播、切另一张照片再按 REC、目标改变：旧会话位所有权与路径不能泄漏或混用。
- 回放中删除虚拟 `nb_movie`：保留现有拒绝码 `0x0432008b`；原 RAW 删除/保护仍调用原厂，跨路径时不 enqueue B sidecars。直接 JPG 同路径仍正常同步。
- 媒体移除/重挂载导致相同 ID 指向新节点：旧会话不得清新节点 bit；open/stat 不再重定向到新卡的同名文件。
- 正常结束、播放器 stop→返回、模式退出、关闭期：保留 V60 延迟清理；没有残留临时 MOV 位或重复清位。

### 真实固件离线运行必须证明的点

- 用仅含 NEF 的 A DCF 分组作虚拟 MOV 锚点时，原厂路径查询和 `pb_info_record()` 的照片记录布局仍能被现有覆盖字段正确支持。
- 在 A RAW 页面读取 B LIVE 文件不要求额外切换原厂卡选择；实际进入 0x56c、解码目标片段、结束回 0x552 且仍选原 A RAW。
- RAW 的原生 OK/删除/保护行为，以及其他普通原厂 MOV 的播放与格式成员，均不受桥接窗口外行为影响。

这些是构建/QEMU 验证门槛；宿主桩测试不能替代 RAW 记录布局、原厂卡选择或实际解码链的验证。

## 证据位置

有效 V60 `runtime.c`：121–141 为 open/stat/info，146–163 为 DCF 位及清理，165–194 为返回/启动，254–295 为 MOV 事实和 lookup，333–357 为 worker/关闭窗口，417–429 为删除/保护同步。原路径为 `build-tools/livediag/feat-onemov/v60-diagnosis/cache-repro/generated/pb-fix37/runtime.c`。

`native_sync.c:39–44` 明确 manifest generation 不按启动次数匹配；`native_sync.c:69–75` 的 native group 检查按 manifest 自己的卡路径运行。因此把跨卡 B manifest 错挂 A RAW 的操作会改变同步对象，不应复用缓存有效就代表可同步的旧假设。
