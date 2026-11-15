# 原厂 REC 的 DCF 配对闭环：实现设计与 ABI 验证

基线为 v52 G MAIN `ed1e8496fccde443412e527a252de5029a55c749053b9b046da42bfc454aa613`。本目录只做独立反汇编及 ARM 实验，没有修改 integration、固件、相机或卡。它给出了可编码的事务边界；尚不能宣称完整原厂登记已在自有 worker 或真机上通过。

## 结论

可以让现有 MOV writer **直接写原厂预留的独立 DCIM/MOV**，以 MAN V2 明确引用其 DCF ID、路径、实际大小，取代新文件的 45 张 JPEG 预览。不要复用照片 ID，不要依赖相邻编号，不要先写 LIVE 再用不可靠的 import helper 搬文件。

提交不能仅看函数返回值，也不能重新取得同一个 file ref 检查 state。可用原厂 `40865210` 从 DCF 索引独立读回 `registered`、ID、大小，配合文件系统实际大小和 MOV UUID，作为 MAN 发布前的成功判据。**此查询可确认当前内存索引；不证明已持久化到介质、掉电原子性或换卡互斥。**

最小实现分三块：①私有 DCF lease adapter；②writer 接受显式目标和提交回调；③MAN V1/V2 分派及原厂 REC 路由。删除/保护必须增加显式 DCF 对象分支，不能让旧 raw-sidecar 删除代码接受任意 DCIM 路径。

## 已验证 ABI

前置分配证据在 `../../audio/registration/RESULT.md`；本次补充见 `native-tests.json` 和同目录反汇编。

| 入口 | 合同及限制 |
| --- | --- |
| `40866c4c` | `(u32 card, u32 kind=5, flags[2], date[6], plan[40], u32 flag=0, out[36])`。plan count@0、16B entry(type@8、size64@16)；out groups@0、12B group(count@4、ids@8)。只接受 card 0/1；本方案单组单 MOV。OK=`04320080`。唯一编号及真实索引存储仍须整机覆盖。 |
| `4085a8e0(id)` / `4085aa7c(ref)` | 取得/释放 file ref。a90c 可以复用缓存；不是独立提交验证。持有 ref 直到所有文件/DCF收尾结束。 |
| `4085bfe0(ref)` | 在内部锁下构造路径，返回 ref+74；32B字符串区及长度@20，总对象36B。`4086e650` 在 G 只是 ret，即返回字符串首址；`4086e654` 读长度。立即有界拷贝，不跨 release 保留这个指针。 |
| `402dadf4(path, stat[0x220])` | 原厂同步文件服务 0xf；返回0才成功，size64在+218。此处已追原调用/ABI；本实验未执行真实文件系统。用它核对 close 后真实文件大小，不能靠 caller 的 mov_end 推断。 |
| `4085c4bc(ref, u64 actual_size)` | 先写 record+18、file state2→1，再 `408647c4` 登记/更新计数。尾返回 unlock 的值，按 void 使用；不允许用 rc 判成功。它的原直接 caller 是虚拟流预登记路径，因此把 exclusive writer 接到它仍需真实 worker lane。 |
| `40865210(u32 id, aligned record[72], u8 *registered)` | 独立读索引：走 `4085fa38`，未经过 file-ref cache。返回必须严格 OK；registered 必须1；record ID@0及size64@18须一致。输出+24的member mask须仅有本 MOV。下层节点定位/存储不是本实验的真实介质。 |
| `4085ac54(id)` | reserved state2/3/8 的取消。内部有临时 ref，外层 lease 的 ref 仍要单独 release；不得解释尾返回为可靠取消成功。state1不取消。 |
| `4085c424(id, flags=0)` | 原厂删除单成员入口；G 首指令已接 PB delete hook。原函数通过 PB 的 original trampoline 保留，不能直接从+4无 prologue 调用。flags1会遍历同组其他格式。内部最终 `40864f20` 的索引失败也不能由外层成功 rc 完全排除，收尾需独立验证。 |

原厂 `4085b320`/`4085b24c` 使用流式/301句柄，当前 writer 使用 `402dd4f8(flags101)` 的 exclusive 句柄。保留当前 writer 的整套 fd API；不把两套句柄混用。分配只取得 DCF 对象和路径，文件仍由 exclusive create 建立。

## 本次实际 ARM 实验

`test_native.py` 执行未改 G 指令，共16组：

- 卡0/1 × 提交成功/失败：执行 c4bc/647c4。下层提交失败时，缓存仍是 state1、大小已改；独立65210/fa38读回 registered0、旧大小。成功时registered1、64位大小9876543210；只有成功发04320028。输出 canary 未变。
- 卡0/1 的真实71cf8/71e98节点转移：清掉 MOV 的 pending bit，写入实际大小。
- 卡0/1 的缺索引节点：registered0，错误04320085。
- 卡0/1 × flags0/1 × protected0/1：执行 ae90/ba30 和记录访问器。flags0仅调用MOV unlink；flags1调用同组MOV、JPG、NEF unlink；protected返回0432008b且零unlink。

明示叶子 stub：锁、节点定位、最终 storage commit（另测真实节点更新）、文件 ref 查找、文件系统 unlink、通知。**16组不能称真实 DCF 文件事务、整机双卡或热拔插已通过。**

## 最小 lease 与事务状态

lease 持有：photo_id、movie_id、card、当前会话 epoch、file ref、拷贝的32B原厂路径、created/closed/commit_attempted/verified 标志、UUID及预期大小。必须把当前 `live_ticket.file_id` 传入 `live_still_ticket`；现有后者尚未保留这个字段。所有 native 调用在保存 worker，不能在 IRQ/UI回调执行，也不能持有 Live ticket/PB journal/UI锁跨调用。

1. **已确认原照片**：使用当前完成票据的 card/id/path/size，重新验证 epoch、对应原照片对象和路径。只读原照片，不改名、不删、不重建索引。检查自身没有并行 mux/cleanup。
2. **预留 MOV**：单卡 kind5，type5，计划大小为已核定上界或原厂允许的实际值（尚未证明 size0 合法，禁止猜0）。严格检查返回、out组数、文件数、ID类型及 card。ref必须是本次reserved对象。拷贝原厂路径，禁止自己构造 `DSC_N+1`。
3. **拒绝已有组碰撞**：独立查询 reserved 记录，必须 registered0、ID吻合、member mask仅有 MOV；电影ID的card/directory/number部分须不同于原照片。原厂 ID 的低4位是格式、card在29..30、dir在19..28、number在5..18；不能只比字符串前缀，因为 movie/photo 的 filename preference 分开。若碰撞，取消本次预留并退出；不盲目改编号或无限重试。
4. **写入**：当前 exclusive101 文件 API 直接创建租约原厂路径，复用已验证 AU/音轨/Nikon metadata/UUID writer。只对新V2取消45帧 `live_preview_write`，保留原厂必需的NCDT/NCTG及小缩略图。原照片复制/Apple pair metadata 按现有逻辑处理，不把此改动扩大为改写原JPG。
5. **关闭与物理文件验证**：所有 fd close 成功；stat成功且 size==mov_end；重新打开，有界解析关键box、sample范围和 Apple content.identifier==事务UUID，检查尾部无截断。取消/epoch变化时不得登记。
6. **登记与读回**：保持 lease ref，调用 c4bc一次。随后独立65210必须 OK/registered1/同ID/同actual_size/member mask仅MOV；重新取得的路径与租约一致，再次stat。不得以重试提交隐藏首次失败。
7. **MAN最后发布**：exclusive创建V2 MAN、写满、close、重新读取并严格解析为完全相同身份。此前不显示Live可播标记。readback成功后才计 transaction成功。出现既有MAN则不覆盖；原照片保持不动。
8. **释放**：归还 lease ref，原子结束任务所有权，恢复预录。readback、错误原因、每一原厂调用的真实返回记入诊断。

## MAN V2 与旧文件

保留现有四字段无version格式为 **V1**，继续原来的 LIVE/MOV+LVP/内嵌JPEG预览路线。新V2只接受如下9字段，重复/未知/缺失/溢出/NUL/CR一律拒绝；version固定2：

```
version=2
native=A:\DCIM\100NCZ_8\DSC_0001.JPG
native_id=<u32>
size=<u64>
uuid=<36字符>
volume_generation=<u32>
movie=A:\DCIM\100NCZ_8\DSC_0002.MOV
movie_id=<u32>
movie_size=<u64>
```

两条路径都29字符且相同card，扩展名严格校验；ID通过原厂解析验证，路径由原厂反解，不能仅做位运算后相信MAN。最坏253B（含换行），仍在现有256B读取上限内。V2 struct增加movie引用和native_id；`same()`、queue identity、lookup cache、GC均要比较新字段，不能只改 parser。V1不得因缺movie字段自动构造相邻ID。

REC启动先验证当前照片ID/path/size，读取V2，再验证movie索引、路径、真实大小、UUID。仅将已验证movie_id交原厂MOV路由；保存精确原照片id和当前UI/media epoch用于返回，取消/换卡丢弃返回请求。删掉/改名MOV后V2不可播，不应猜其他文件。旧V1仍可读；旧固件遇V2应拒绝显示，不进行降级解析。

新MOV在DCIM，不再有同内容LIVE/MOV副本，因此导出工具需按V2引用定位，不能继续假定 `LIVE/<photo stem>.MOV`；否则会造成“机内可播、导出找不到短片”。这不是增加另一份MOV来回避的问题。

## 失败回收与断电边界

| 失败阶段 | 允许动作 |
| --- | --- |
| reserve前 | 释放内存；不碰任何DCIM文件。 |
| reserved、未创建 | 当前同epoch且lease ref/ID仍吻合时调用原厂cancel；验证reserved索引消失。 |
| 自有文件已创建、未commit | 先完成/确认本次fd关闭，再走reserved cancel，检查文件缺失及索引不再预留。close不确定时保留lease为待恢复，不能同时unlink仍活动writer。 |
| commit尝试失败 | **缓存可能已是1**，不能盲用ac54。以当前 lease 及独立读回区分；走已审核的单成员原厂删除收尾，随后文件与索引均确认缺失。此部分仍需真实失败注入覆盖；无法确认则保留孤儿，绝不伪报清理成功。 |
| registered但MAN失败 | 删除本次已创建的MAN/PAIR.JPG仅在身份明确时；本MOV使用原厂delete(id,0)，不能rawremove DCIM。禁止flag1。已被用户保护则不解除保护/强删，保留可识别孤儿。 |
| epoch变化/拔卡/ID身份不符 | 停止所有基于旧id/path的删除；不可拿旧lease去清新卡同名文件。不在新卡执行“补清理”。 |

MAN-last使有效MAN只指向完整、已核验MOV；**它不提供掉电原子性**。断电可能留下完整孤儿MOV或未完成文件。若要自动恢复，必须用小型持久pending记录且包含完整租约身份，冷启动重新验证实际MOV UUID与索引再清理；当前内存sync journal不持久，不能冒充它。最小第一版可以保留未确认孤儿并报告，而不能牺牲原照片做清理。

删除返回成功仍需双读回（FS缺失、DCF该成员缺失），不能把registered0当“根本没有reserved节点”；unknown/error≠absence。必须区分缺失错误、卡未就绪和I/O失败。

## 双卡、同名与保护

每张已完成原照片票据独立预留自己的MOV，独立ID/路径/lease/MAN。B卡只从已验证A卡MOV读源字节，目标重新exclusive写、stat、commit、readback；不能改首字母复用A的MOV id/path。B失败不撤销A成功。双卡票据是否同一照片仍按现有ticket/generation/press关联，不把任意同名文件当twin。

当前 slot 指针并非介质身份：`40863eb8..3f04`从固定RAM arena `a149e6dc0`、stride`222188`初始化 `a04822af8[0/1]`，后续槽初始化复用这些对象。pointer相等不证明卡没换。运行期epoch用于取消；持久MAN中的volume_generation只记录写入会话，不能跨重启当卡身份。换卡与原生DCF调用的串行化/有效ref寿命尚待实际契约验证；前后检查epoch本身不是互斥证明。

V2不主动创建同号JPG/MOV组。原厂group逻辑和flags1会把同目录/编号的格式关联；它会影响照片显示类型、删除和保护，已不能作为“简单配对”。既有同组文件或第三方手动复制碰撞应拒绝自动所有权；仍保留原厂正常浏览行为。

V2同步需独立分支：原照片的JPG/NEF组仍有成员时保留配对；只有原照片组确实消失且V2/UUID身份匹配，才删除其独立MOV(flags0)。原厂用户单独删除MOV时使V2播放失效，保留原照片。保护沿原厂单成员协议处理，必须先读回真实保护状态；不沿用旧raw sidecar“先清readonly再删除”，也不自动解除用户单独设置的MOV保护。旧V1 confined sidecar删除范围保持原样。

## 尚需的最小整机验证

本次允许进入**隔离实现与QEMU登记实验**，不等于可打包/刷机：

1. 一个真实照片票据后，原生reserve→当前exclusive writer直写→close/stat→c4bc→65210→MAN V2；观测实际编号与原照片保留、DCF浏览可见，REC按MAN精确movie_id播放并返回精确原照片id。
2. 独立虚拟卡A/B、已有同号JPG/NEF/MOV以及自定义电影前缀；证明allocator不会由实现强行复用，碰撞时安全取消。
3. 写失败、close失败、存储索引提交失败、MAN已存在/写失败分别注入；全部保留原照片，verify rollback，未知状态保留孤儿。
4. 介质换代发生在reserve/write/commit/rollback边界，证明不会操作新卡同id；原生锁/回调上下文还要结合真实时序，不仅ARM mocks。
5. V1原有REC/删除/保护不回归，V2删除/保护及冷启动重新扫描通过；完成后才从新writer移除45JPEG生产路径。

重跑本次局部实验：`DCF_IMAGE=<exact G main> /path/to/user/.local/share/nikon-z8-qemu/venv/bin/python test_native.py`。完整映像SHA硬断言，结果写同目录native-tests.json。
