# V61：复用原厂双卡副本关系，精确读取 JPEG 成员

2026-11-15。只读逆向和设计；未操作 USB、相机或存储卡，未修改生产源码。本文给出静态反汇编证据，不把宿主 stub 或原厂 API 的存在当作相机实测。

## 结论与最小接口

原厂“Jump to copy on other card”使用 `0x403fb59c(id, 2, &out)` 查询副本，再把输出 ID 写入设置 `0x4910`。V61 只复用查询，不执行原厂切图动作；显示/返回仍保留原 RAW ID，目标 JPEG 只提供已经验证的 MAN/MOV。

```c
/* caller initializes out = 0; invoke from worker with no outer DCF/GUI lock */
uint32_t query_peer(uint32_t id, uint32_t mode, uint64_t *out); /* 403fb59c */
/* accept only (return_value & 1) && (uint32_t)out && !((out >> 32) & 2) */

void *get_exact_ref(uint32_t id);             /* 4085a8e0 */
const char *ref_path(void *ref);              /* 4085c0d0 */
void put_ref(void *ref);                     /* 4085aa7c */
```

精确 JPEG 查询使用 `get_exact_ref(id & ~15u)`，成功时调用 `ref_path`，在 `put_ref` 前复制路径，再验证 `.JPG`、完整 MAN native_path、实际原片大小和 MOV。三个 ref 接口各自加/解 DCF 锁。不要在外层持有 DCF 锁再调用。

**不能使用 `407b95e4(id & ~15u, path)` 强制 JPEG。** 它调用的 `4085cd20` 在 `4085cd44–4085cd50` 把格式强制改为 `0xe`，随后选同组优先成员；RAW 优先时仍可能返回 `.NEF`。这个问题同时影响同卡 fallback 和原厂返回 peer 后的 JPEG 解析。

同卡 fallback 仅用于原本显示 `.NEF` 的照片；用精确 format 0 ref 验证原厂 DCF 同组 JPEG 成员存在，再做 MAN/文件验证即可。无需依赖原 RAW ID 的低 4 位必须等于某个固定 RAW 类型：当前选择可能是组 ID，原厂路径扩展名与精确成员查询更合适。

## 查询契约与原厂调用链

- 英文文本 `Jump to copy on other card` 位于 `0x432b4290`，text ID `0x6c0`，UI resource `0x4685`。相邻 `0x46a5` 实际是 `Choose slot and folder`，不能混用。
- 原厂执行函数 `4062dce0` 在 `4062dcfc` 读取设置 `4910`；在 `4062dd0c` 调 `403fb59c(id, 2, &zero_u64)`；输出低 32 位非零时在 `4062dd1c` 写入设置 `4910`。此函数本身会切图，V61 不应调用它。
- `403fb59c` 打包 `{uint32_t id, uint32_t mode}`，经 `403fa1b8` 发送 command `84 / 0x54`，dispatch `440e2fe0 -> 4078d0c0 -> 407ac284`。
- `407ac284` 先清输出 8 字节。在 `407ac3d8` 调 `406bd300`，失败保持零；在 `407ac3e4` 拒绝 flags bit 1；在 `407ac3ec` 用 `stp w9,w8,[out]` 写入 peer ID 与零扩展 flags。**flags 是高 32 位，不是简单把内部 byte4 原样 memcpy 8 字节。**
- `403fa1b8` 的低 bit 1 表示同步命令完成；没有可接受副本时仍可能返回成功而 `out == 0`。必须同时检查返回值和输出。
- mode 2 查询还受当前原厂媒体/回放状态门控，不应当作可离线枚举任意 ID 的纯函数。查询实际当前照片，结束后重新检查媒体 epoch、shutdown 与选择状态。

## 配对依据与保证边界

`406bd300 -> 406cc6e0 -> 4085ce68` 复用原厂副本关系。不是 V61 手动切 card bit 或仅按相同文件名猜配。

`4085ce68` 解出卡、目录和文件号，读取源目录的实际后缀，在目标卡上做 DCF 搜索。成功取得两边原厂 refs 后，还执行原厂记录状态判断。该层返回结构低 32 位为候选 ID、byte4 为分支控制、byte5 为目标 flags。

具体必须保留分支含义：两边相关记录状态可用时，`4085cff8` 比较记录字段 `+32`（`4086dbe8`），不等即失败；相等时 byte4 写 0。外层 `406cc6e0` 对 byte4==0 再分别经 `406cc790` 读取 23 字节原厂元数据，并在 `406cc774` 比较全部 23 字节；相同才接受。另一个原厂允许分支将 byte4 写 1，外层直接接受候选及 flags，**不进行这次 23 字节比较**。

因此准确保证是“遵循相机原厂跳至副本的身份判断”，不是“每条分支均经过 EXIF/UUID 强校验”。23 字节字段尚未逐字段解码，不能声称它一定是 UUID、拍摄时间或机身序列号。同卡 NEF/JPEG 通过精确 format 0 ref 证明原厂 DCF 同组成员；不能承诺识别人为替换后仍被原厂纳入同组的文件。

## 为什么精确 ref 能读取 JPEG

`4085a8e0` 在内部 DCF 锁下调用 `4085a90c`，保留请求 ID 的低 4 位，经 `40865210 -> 4085fa38` 解析。

- `4085fa98` 解请求格式；只有 `4085fad0` 的格式 14 分支才用 `4086d1c8` 选组优先成员并替换请求格式。
- 明确格式 0 保持 JPEG 请求。已有成员记录经原厂索引读取；需要进一步确认的成员在 `4085fc98–4085fcd0` 构造请求格式路径、stat 并更新记录；失败则 `4085a90c` 释放并返回空。
- `4085fbc8` / `4085fc70` 用 `4086d390` 把请求 ID 写入 ref 内记录。`4085c0d0 -> 4085c018 -> 4086d54c -> 40864648 -> 4085e770` 使用这个精确 ID 取得路径。不会再次强制为格式 14。
- ref 非空后必须释放；路径应在释放前复制，不能长期保存原厂字符串指针。

原厂 ID 编码由 `4086cd04` 等直接显示：卡 bits 29–30，目录 bits 19–28，文件号 bits 5–18，格式 bits 0–3；bit 4 不在此次格式替换范围。使用 `id & ~15u` 保留 card、directory、file number 和 bit4。

## 锁、等待与 V61 建议

原厂 `403fa1b8` 在 `403fa3c0` 取得全局命令互斥锁，`403fa7dc` 入队，在 `403fa824 / 403fa878 / 403fa8c4` 的等待使用 `-1`，没有已证实的本地超时；`403faa3c–403faa40` 解锁。只能在 worker 查找阶段、未持 DCF/GUI/journal 锁时调用，不能放进按键/绘制回调。相机实际调度时序尚未验证。

建议查找顺序：当前路径直查 MAN；若原片是 NEF，再查同卡精确 JPEG；若无有效 Live，再查原厂 peer，并取 peer 的精确 JPEG。每个候选都要保留 MAN 完整路径与实际大小校验，最后验证 MOV。拒绝 peer 为零、flags bit1、非法卡号或仍为同一卡。

`native_path/current_id` 始终代表原显示照片，`manifest.native_path` 代表验证后的 Live 所属 JPEG。桥接仍以原 RAW DCF 节点作虚拟 MOV 锚点，`nb_from` 为源照片同组 `.MOV`，`nb_to` 为目标 MAN 的 Live MOV；退出恢复原 RAW。只有 displayed path 与 manifest path 完全相同时沿用原有 sidecar 删除/保护同步。

## 证据来源与版本核对

反汇编文件：[pairing-native-disassembly.txt](./pairing-native-disassembly.txt)。输入与逐段 SHA/交叉核对：[pairing-provenance.json](./pairing-provenance.json)。本地 flat MAIN 是 `codex-w5/bg34.main`，基址 `0x40160000`；不能把它整体称为未修改的官方固件。

独立解码交付 V60 `delivery/Z8-v60-Live-OneMov-641725ce/Z_8_0406.bin` 后核对本文 15 段：14 段逐字节相同，剩余 `4085fa38..4085fd28` 仅 `4085fcc0` 一条 stat 调用不同，正是现有 V60 `pb_stat_wrap` 桥接 hook。原厂请求格式/成员选择控制流未变。flat MAIN SHA-256 `c7c6dbeea8a94064a10d47a7b9022ab26e4aa6b12ba5fb63b1ad85abac4fe4d4`；交付 V60 MAIN SHA-256 `641725cec49a702a9440e790a6495b6aef4fb0f7ef5a71b6acbda2719ba47d49`。

## 后续只读审查：副卡变化后清理源 RAW 节点

当前仅用全局 `media_epoch` 约束清理，会在 B 卡发生变化时连 A 卡原 RAW 节点也不清理，遗留本次添加的 MOV bit。反过来，直接删掉 epoch 条件、仅比较旧 node 地址也不安全：原厂 slot 对象中节点可能复用。

原厂卡事件已经携带卡槽，可自行维护 `card_epoch[2]`，不必臆测未解码的原厂 generation 字段。证据：[card-events-disassembly.txt](./card-events-disassembly.txt)、[card-events-provenance.json](./card-events-provenance.json)。

| 事件 | 原厂名称 | 卡槽字段/证据 |
|---|---|---|
| 22 | CardInsert | `message+40` 的 uint32，`40d62e68` |
| 23 | CardRemove | `message+40` 的 uint32，`40d62e74` |
| 24 | CardMountComplete | `message+40` 的 uint32，`40d62e88` |
| 25 | CardMountError | `message+40` 的 uint32，`40d62eac` |
| 26 / 27 | CardUnmountComplete / CardUnmountError | `message+40` 的 uint32，`40d62d10` |
| 28 / 29 | CardFormatSyncStart / CardFormatSyncEnd | 生产端 `40d63110` 经 `40d634b8` 得到槽号，调用同步发送 `40d60e94` |
| 30 / 31 | CardFormatAsyncStart / CardFormatAsyncEnd | 同一生产端，调用异步发送 `40d60e64` |

名称来自 `43f907f8` 的 88 字节事件表。Card controller 是 `40d62ce0`，其 `43f93340` 跳转表用事件号减 20 索引。事件 24 **只是挂载完成，不是所有拔卡事件的统称**。

槽号是 0/1：插拔事件生产端 `40d633c8` 对回调参数零/非零规范为 0/1；挂载/卸载完成生产端 `40d62c18` 通过 `40d634b8` 把底层槽号规范为 0/1。对应状态地址 `0xa03417bc8 + 104 * slot`；public selector `40d62b18` 也只接受 0/1。这些状态字段已见的是状态/标志/结果码，未证实任何单调 generation。

hook 能看见全部这些事件：`pb_system_entry` 位于通用 dispatcher 的 `40d60ecc`，先 `save_all -> pb_system_notify`，然后恢复原寄存器/指令并继续 `40d60ed0`。原厂到 `40d60ee0` 才读事件号并分发，没有在 hook 前只筛 7/24。上游 `live_ui_system_dispatch` 也对 22..31 全部调用 `live_old_system_dispatch`；它对 7/24 的特殊处理不是过滤其他事件。`40d60e64` 把 event 写入 message+32，把 x1 参数写入 message+40，再排队；同步发送 `40d60e94` 使用同一布局。新增 9 段反汇编与交付 V60 核对，仅 `40d60ec8` 上游已有 Live UI hook 的分支地址不同，其余字节一致。

建议最小清理策略：

1. 对已证 22..27 生命周期事件，**先递增全局 media epoch，再递增指定槽 card epoch**；若覆盖运行中格式化，再包含已证 28..31。非法槽号保守使两张卡都失效。这是两次原子操作，不能称为原子地同时递增。
2. 开始桥接时保存源 RAW 的卡槽及该槽 card epoch，并保留源 slot/node 身份。重定向/缓存继续受全局 media epoch 限制。
3. 清理时仍持 DCF 锁和 slot 锁，通过当前 slot 和原 RAW ID 重新找节点；仅源卡 epoch 相符、节点位于当前 slot 范围、node 地址与本次实际设置位置相符时清除自己添加的 bit 5。只拔 B 时允许清 A；换 A 时禁止触碰新 A 的同号节点。不要解引用旧裸指针。
4. worker stage 2 的 `gone` 条件包含 `nb_epoch != media_epoch`，使用现有 1 秒期限进入清理。否则即使媒体已失效，只要设置 4910 仍是 nb_movie，就会不断把 idle 时间归零。

时序边界：23 在通用 dispatcher hook 之后才由 Card controller 发起其卸载状态转换/请求（`40d63010 -> 40d63604/40d63528 -> ... -> 40d687fc -> 40865ef8`）；24/26 则由挂载/卸载完成回调发出，属于完成后的通知。不能据此声称所有底层硬件变化一定晚于 hook，或完成事件一定先于每个索引指令；早期插拔与完成事件共同推进本卡代次，结合 DCF 锁与重新查节点，提供保守边界。需真实相机验证拔副卡和源卡换卡场景。

最后只读复核的生成 runtime SHA-256 为 `2e09c019fa19fbf46da71100b222f4a33b1c10e368157b636290510508ef7dbc`。`nb_start` 在 DCF 锁内先保存 `nb_card_epoch`，再检查 lookup/global epoch 后设置 bit；`nb_mask` 清理在 DCF+slot 锁内重查 node，紧邻写入检查源卡 epoch；`nb_end` 不再因仅副卡变化跳过清理；`pb_native_return` 用源卡 epoch 与当前同组选择防止恢复到新卡照片；worker stage 2 已将全局 epoch 变化纳入 1 秒清理期限。

最终 notify 顺序已改成 global 在前、card 在后。这样 `nb_start` 先 acquire 读取 card epoch，再 acquire 读取 global epoch 时，若看见该事件的新 card epoch，也必须看见之前发布的 global 递增，旧 lookup epoch 就会被拒绝；若夹缝中读到旧 card epoch，稍后的递增会保守阻止本次清位。此前相反顺序存在“新 card epoch / 旧 global epoch”窗口，现已关闭。此内存顺序结论不扩大为底层硬件或所有原厂索引重建时序的实机保证。

## 对 lookup 草稿的只读审查

2026-11-15 已检查使用精确 ref 的 `lookup.cfrag`：先复制路径再释放 ref、peer return/flags/异卡检查、MAN 全路径与大小验证符合上述证据。一个明确但非本场景阻断的行为边界：同卡 JPEG 的 MAN 有效而其 MOV 不合格时，当前在候选选完后统一 `mov_size` 并返回错误 3，不继续尝试另一卡；如以后需要容错，应把完整 MOV 可用性纳入每个候选的接受条件。

## 删除/保护快照的限定检查

按主 agent 后续要求，只读检查 `pb_delete_wrap` / `pb_protect_wrap` 到实际同步步骤，未改代码。`work/pb-fix37/runtime.c:464` 起的 valid 检查与 `m=r->manifest` 没有共用 `pair_lock`；相同快照方式已存在于 `baseline-v60/pb-fix37/runtime.c:417` 起。`enqueue` 对 id 仅检查非零，不把它保存进 journal，也不再次验证 id 与 manifest path 的绑定。不能宣称 journal 的身份校验完全排除了完整换成另一份有效 MAN 的快照。

但现有步骤提供了实质限制，而非直接传播原操作：`sync.c:78` 重新读 MAN，要求 path/size/uuid/volume_generation 与队列身份全部一致；`native_sync.c:123` 在 DCF 锁内完成扫描与修改。DELETE 若该队列目标仍有任何原片成员，`sync.c:79` 直接消费队列而不删 sidecar。PROTECT 不保存或使用原调用的 protect 参数，而在 `sync.c:74,86` 读取**队列目标自己**的保护状态，再同步该目标的 sidecar；`native_sync.c:81` 发现属性已一致即不写。

可构造的条件性交错是：valid(A) 通过 → worker 切到 B 并完成写入 manifest(B) → wrapper 复制完整 B → 原厂 protect(A) 成功 → enqueue(B)。若 B 的原片未保护、其 sidecar 原先却为 readonly，步骤会对 B 调 setattr 清 readonly；相反方向同理。这说明有“处理了另一目标”的同步缺口，但还需要 B 本来属性不一致，才会真的写入，且写入值来自 B 自己，不能据此声称 A 的保护意图被施加给 B。DELETE 要实际删除 B，还必须 B 的原片组已经全部不存在；有原片的 B 受成员检查保护。

原厂 delete/protect 入口各自在 hook 之后才于 `4085c44c` / `4085bcd0` 加 DCF 锁，尚未找到能证明 valid+copy 全程线程串行的证据；同样也未证实实际 UI 能在这些指令之间切换当前照片。当前结论是已有的未同步快照边界，未证成正常 RAW→副卡 Live 操作导致错误删除或错误保护。此次不据此扩大生产修复范围。
