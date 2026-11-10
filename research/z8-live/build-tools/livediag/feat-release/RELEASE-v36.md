# Z8 Live v36 源码发布

本次交付是从指定 v32 源码生成的正式版候选源码，未生成可刷写包、未执行实验室 chain、未进行 v36 真机回归。没有连接 USB、刷写、操作相机或运行 QEMU。参考目录只读，源码修改及交付文件位于本目录。

## 改动与边界

- `src-fix36/` 完整复制自 `src-fix32/`；`pb-fix36/` 完整复制自 `pbfix/live-playback-ui-icon/`。完整差异见 [src-v32-to-v36.diff](src-v32-to-v36.diff)、[pb-v32-to-v36.diff](pb-v32-to-v36.diff)，逐文件前后 SHA-256 见 [source-manifest.json](audit-v36/source-manifest.json)。源树自带的历史 `.o`、日志、测试、`rerun/` 等随副本保留，**不是本次构建结果**；本次对象全部在 `audit-v36/src/`、`audit-v36/pb/`。
- Wave5 pool 内原 `close_lock` 改名为 `reserved_close_lock`，不再作为锁访问。实际锁放在缓存堆 `live_runtime.w5_close_lock`，复用原 `log_pad`。`live_w5` 尾部填充保存 `rmw` 指针。初次分配前获取并检查 runtime 的缓存地址窗口 `[0xa00000000,0xa40000000)`、8 字节对齐、magic、size 及 encoder 归属；在发布 `e->hw` 前绑定指针。锁由 runtime 开机清零一次，close/reopen/reuse 不重置。
- 沿用 v9b 的“pool 保留、只迁移 RMW 字段”方案；v32 没有 `diag_n`，没有加入该字段。锁的 acquire/release 次序、竞争延迟与超时、原 stock close 流程保持不变。pool 分配次数、请求字节数计算、各 DMA region/slot 排列不变。`busy`/`release_req` 仍在 pool，使用普通原子 load/store（LDAR/STLR），没有独占循环。
- `runtime.c` 与 v32 **逐字节一致**，保留照片→录像切档不主动关闭编码器、由 bring-up hook 交还 Wave5 的行为。`capture/`、`audio/`、原 `hooks.S` 和其他 codec 文件也保持不变，保留原有竖屏预览、音频增益和 Live 功能。
- 未从 v9b 引入 recorder、`fr_*` 调用、lamp、W5F/W5L 诊断文件或 D/P/I 码。v32 本来已有 `live_fr/fr_head`、日志计数与 RAM metadata，原样保留；源码差异脚本检查新增行没有带入上述实验记录器代码，不能把“未引入”误读成删除了全部 v32 历史诊断字段。`W5L1` 是原 encoder magic，不是新增文件。
- 图标应用当前 `../feat-icon/` 选定的 SVG、生成器和 patch。当前 geometry 为 **110×44**（README 正文的 112×44 已落后于素材），拍照 `center_x=54`、`y=98`，回放 `x=y=24`，目标尺寸也为 110×44，无 target overrides。原 geometry 的 delivery 标记为 preview-only，故重新生成 C 头文件，不能复制其旧头文件。
- [icons-v36/geometry-reference.json](icons-v36/geometry-reference.json) 保存原 geometry；[生成 geometry](icons-v36/icon32-assets/geometry.json) 的尺寸、锚点、design、raster、targets 与原件相同，仅 delivery 更新为 `assets-and-headers`。复用第三版代码绘制素材，没有模型生成图像。图标实现保留原 patch 的透明边缘、busy 扫光、视口重排、目标失活重试和 gate/readers 保护。
- `icons-v36/icon32_patch.py` 仅调整输出目录保护，从图标目录扩展到当前 release workspace；七个输入源码哈希检查和补丁内容不变。哈希检查全部通过，证明本输入满足 `src-fix31 + v32_patch` 的图标前提。没有再对已是 v32 的 runtime 重跑 `v32_patch.py`。

### diff 摘要

|层|修改文件|新增文件|
|---|---|---|
|Live|`codec/live_w5enc.c`、`runtime.h`、`ui/live_ui.c`|`ui/icon32_shoot.h`、`ui/icon32_layout.h`、`ui/icon32_alpha.h`|
|回放|`ui.c`、`ui.h`、`runtime.c`、`runtime.h`、`native_ui.c`、`test_ui.c`|`icon32_pb.h`、`icon32_layout.h`、`icon32_alpha.h`|

共 Live 6 个、回放 9 个变化文件，其余文件与输入相同。回放修改均来自图标补丁，无额外 Wave5 诊断改动。

## ABI 与工具影响

AArch64 clang 对象导出和完整 record layout 比较见 [layout/results.json](audit-v36/layout/results.json)、[record-layout-36.txt](audit-v36/layout/record-layout-36.txt)。

|结构/字段|v32|v36|
|---|---:|---:|
|`sizeof(live_runtime)`|23184|23184|
|`log_pad` → `w5_close_lock`|18908 / 0x49dc|18908 / 0x49dc|
|runtime.capture / stream 偏移|8 / 2880|8 / 2880|
|`sizeof(live_w5)`|1053600|1053600|
|`live_w5.rmw`|尾部 8 字节填充|1053592 / 0x101398|
|Wave5 header 页对齐大小|1056768|1056768|
|`sizeof(UiState)`|18488|37920|
|UiState magic / metadata version|0x4c505549 / 1|0x4c50554b / 3|
|UiState engine / display / gate 偏移|8 / 12072 / 18144|相同|
|`sizeof(PbRuntime)`|8240|8400|
|PbRuntime display / gate / render_request 偏移|1856 / 32 / 8196|相同|

runtime 所有既有字段及嵌套字段偏移均经 record-layout 对比一致（只规范化目录名及 `log_pad` 名称）。不需要因锁迁移改写 UI→engine 地址或扩大 runtime；原 `log_pad` 已有语义，工具不得再将其视作可随意清零的填充。pool 原锁位置不再控制 close，旧工具往该位置写 1 已无效果。

图标会扩大两个 UI 所有者对象，要求冷启动并同时部署两层；读取工具若校验 magic/精确大小，必须接受上表新值。原有 UI 字段偏移仍保持不变，新字段按最终 ELF 中 `live_ui_metadata` / `icon32_metadata` 读取。v9b recorder arm/解码工具面向诊断版 runtime，**不适用于本版**；本版未提供 arm 接口，不应按诊断版 26512 字节结构寻址。

## 原子 RMW 审计

[全树源码逐项清单](audit-v36/source-rmw.md) 包含每个 CAS/fetch/exchange 站点、对象及内存来源；[194 条对象指令清单](audit-v36/exclusive-audit.md) 包含每条 LDAXR/STLXR/LDXR/STXR 的对象名、函数相对偏移、寄存器、源位置与基址来源。[JSON](audit-v36/exclusive-instructions.json) 另附实际对象指令上下文；完整 `.s` 在对象同目录。

|对象|RMW 成员|内存来源与结论|
|---|---|---|
|live_runtime|card_epoch、full_presses、crumb_head、writer_active、writer_returns、ticket_lock、ticket_drops、stream.job、w5_close_lock|`live_runtime_boot` → `0x40d2c408(sizeof,0)` 缓存堆；state()/worker 参数传递|
|la_capture|cancel_epoch、irq_seq、press_mailbox|runtime.capture 内嵌缓存堆对象；像素指针所指 pool 不等于 capture 自身|
|live_stream|log_head|runtime.stream 内嵌缓存堆；源码的 live_log 内联到多个调用者|
|UiState|usb_events、standby_kept、reload、command、display.readers|install → `0x40d2c408` 缓存堆；图标 gate 是同对象上的单 UI 写者 load/store，不再 fetch_or/and|
|FxRegistry|live_ui、playback、menu_lock|`fx_registry` → `0x40d2c408` 缓存堆|
|core.events[46]|registry 发布槽 CAS|core 常驻对象 +2528；由缓存 `0xa` RAM 窗口检查后的 root 链取得。root 指针锚点为静态 RAM `0xa032b50d8`，不是直接对该锚点做 CAS；core 的实际分配实现不在本输入内|
|PbRuntime|gate、render_request、journal_lock、key_epoch、media_epoch、display.display.readers|pb_boot → `0x40d2c408` 缓存堆；图标新增/替换的 CAS 均在此|
|live_w5|原 close_lock|pool `0x8` 非缓存别名；已迁移。对象仅 4 条独占指令，全部在 w5_close 的首试/重试路径：普通 LDR 读取 `w+1053592`，再加 runtime `18908`，LDAXR/STLXR 操作该缓存字段|
|公共头文件未实例化 helper|两份 fenestra_runtime_accounts.h 的 state.flags、fenestra_native_ui.h 的 tag|本版两层没有调用站点、未生成指令。是调用方提供的 worker/service/account 存储，单凭声明无法证明具体分配类型；不归类为已验证堆对象。将来实例化时必须重新审计，不能传 pool/MMIO 指针|

`runtime.c`、`capture/live_stream.c`、`capture/live_capture.c`、两份 registry、PB runtime/native_ui 及新增图标头均已覆盖。其余 shipping 单元未发现 RMW。扫描还覆盖所有可执行 ELF section，确认没有函数外遗漏独占指令、LSE RMW 或 `__atomic`/`__sync`/`__aarch64_*` 外置 helper；本目标不启用 LSE。无外设 MMIO RMW。pool 的 retained `fr` 数组是单写者记录写入，`fr_head` 发布使用 store，既有 audio 原子也是 load/store。

上述内存类型结论基于分配调用链、已知地址别名及用户提供的真机根因，不是本次测量 MMU。根因依据读取了 `../feat-rec/runs/v8b-4996/report.txt`、`FIX17-RESULT.raw` 和 `runs/v9b/v8b-source.diff`：视频报告本身只能约束调用前后范围；v9b 在相机上 REC/文件恢复正常是用户本轮提供的事实。本次只对 v36 源码与对象作离线核验。

## 本地验证与复跑

在本目录运行：

```sh
python3 -B tools-v36/compile.py
../feat-rec/.recorder-venv/bin/python -B tools-v36/layout.py
../feat-rec/.recorder-venv/bin/python -B tools-v36/audit.py
python3 -B tools-v36/check_icons.py
../feat-rec/.recorder-venv/bin/python -B tools-v36/link.py
python3 -B tools-v36/delivery.py
```

`compile.py` 用本机 clang 21，以 `relive_w5.py` 的 AArch64 `-Os`、freestanding、general-regs-only、strict-align、宏定义和 `-Wall -Wextra -Werror` 编译 **18 个 Live + 10 个 PB shipping 单元（含两份 hooks.S）**，全部成功且无诊断。PB 的 `ui.c` / `native_ui.c` 按图标 README 改用 `-Oz`，其余保持 `-Os`。精确命令和结果见 [commands.json](audit-v36/commands.json)、[compile-results.json](audit-v36/compile-results.json)。不把备份 minih264 encoder、旧 rerun/QEMU observer、host 测试 main 或 Apple `._*` 文件冒充固件构建单元；这些是原树历史材料，未链接到本版。

原子扫描解码的是不带 debug 的生产对象；另编译 `-g -S`，逐函数匹配全部独占指令的数量、顺序、opcode 和寄存器，再定位源码。不会把 LDAR/STLR 算作 RMW，也不会把 host 模拟当作真实缓存属性测试。

[图标测试结果](audit-v36/icon-tests/results.json)：拍照常态 +24 相位+循环首帧的 26 帧逐像素比较、PB 输出逐像素比较、视口/显示目标/姿态重挂载、离开重返、取消期间 gate/readers、挂载/摘除失败重试、透明度与 foreign plane 保护均 PASS。测试复用提供的 fixtures，只把旧 112 宽断言改为实际生成尺寸宏；宿主 stub 不证明机身调度或面板结果。

[本地链接结果](audit-v36/link-results.json)：使用已审计对象、参考 MAIN SHA `c7c6dbeea8a94064a10d47a7b9022ab26e4aa6b12ba5fb63b1ad85abac4fe4d4` 与匹配 manifest，只生成 ELF，没有改写 MAIN。Live 81246 字节，在 `0x401e2000..0x401fe000` 留 33442 字节；PB 35754 字节，在 `0x40196000..0x401a0000` 留 5206 字节。检查旧范围/空白余量、无 undefined symbols、无 `.data/.bss` 可写静态状态、15/10 个 hook 目标及 B/BL 跳转可达。使用本地 Zig 携带的 ELF linker；对象仍全部由 clang 生成。对象排列/工具链与实验室可能不同，大小和地址需在实验室最终产物再核对。

`layout.py` / `audit.py` 需要 pyelftools、capstone；`link.py` 还读上述参考 MAIN/manifest，优先 ld.lld，无该命令则使用现有 Zig。`prepare.py` 仅用于原始输入仍在、`src-fix36`/`pb-fix36` 尚不存在的全新目录；会拒绝覆盖现有副本。正常复编无需重新生成或打补丁。

## 实验室构建命令（本次未执行）

把 `src-fix36/`、`pb-fix36/`、`tools-v36/compiler/` 及本说明带到实验室 `live-audio` 工作目录。沿用实验室已验证的 merged MAIN 与配套 manifest，不混用其他版本的文件；设置 `V36_BASE` 和 `V36_MANIFEST` 为它们的路径。

```sh
cd "$HOME/.local/share/nikon-z8-qemu/live-audio"
: "${V36_BASE:?指定匹配的 merged MAIN}"
: "${V36_MANIFEST:?指定该 MAIN 的 manifest}"
export V36_REAL_CLANG="$(command -v clang)"
export PATH="$PWD/tools-v36/compiler:$PATH"
python3 relive_w5.py src-fix36 "$V36_BASE" "$V36_MANIFEST" build-v36
PB_SRC=pb-fix36 bash w5in/chain.sh build-v36 chain-v36
```

`tools-v36/compiler/clang` 是小型编译参数适配器，只对路径包含 `pb-fix36` 的 `ui.c`/`native_ui.c` 选择 `-Oz`，其他参数透传真实 clang，避免旧 `pbin/relive_pb.py` 固定 `-Os` 漏掉图标 README 的要求。`chain.sh` 会切换到上述实验室目录，删除/重建传入的 `chain-v36` 输出目录，并顺序运行回放、USB debug 默认开启、GUI pool fix；本次仅参考该脚本，没有更改它。其 `pbin/relive_pb.py`、`w5in/usb_default_on.py`、`w5in/gui_pool_fix.py` 与实验室 venv 并不包含在本交付中。

链末的 `chain-v36/pool/pool.main` 是实验室脚本产物。旧 chain 并未为 pool 步骤写出完整累计 manifest，不能用 usb 步骤的 manifest 当作最后 MAIN 的证明；实验室需保留每步输出、最终哈希及最终范围检查，并重新核对 payload 不越过保留 helper 区。本次 ELF 检查不能替代最终链式 MAIN 的验证；没有执行打包/刷写命令。

## 待真机回归清单

- 冷启动后 Live 拍照：开关、连续多张、忙碌动画与保存完成；照片关联 Live MOV/LVP 可用。
- 横持、左右竖持的 Live 回放；LCD/EVF 切换，进入/退出回放、遮挡/重挂载，图标尺寸位置与透明边缘；原 LVP 预览布局不被徽标覆盖。
- 音频可听、增益符合 v32 行为、音画时序、短片与连续多张保存。
- 照片 Live 工作后切录像档：切档仍不关闭编码器；REC 开始、停止、连续再次开始停止，无卡死/重启，录制 MOV 完整可读。
- 回放刚录制的原厂 MOV，退出后回照片模式继续 Live 拍照及 Live 回放。
- 照片→录像→原厂 MOV 回放→照片，多轮往返；包含 Live 开/关、拍照忙碌/空闲时切换，观察重建、超时与文件完整性。
- 冷启动后马上插 USB、待稳定后连接、断开后继续 Live/REC；核对连接时序、UI 状态及无重启。这里只列后续人工回归，本次没有 USB 操作。

以上全部为 v36 待测项。用户报告的 v9b 真机成功不等于本次合并新图标后的 v36 已通过真机回归。
