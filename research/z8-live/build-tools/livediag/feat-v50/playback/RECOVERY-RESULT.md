# v51d REC 预览与原厂取景恢复

本文保留旧 profile 的失败记录。后续针对性分支定位及既有 secondary-stop 模型对照已完成，同固件在固定修正 profile 中完整回归通过，最新结论见 [A2-MODEL-AB.md](A2-MODEL-AB.md)。

在本文记录的原轮次，候选 `aa6e94e0042fb445c666de73c0f6b9e7e569010b7993a3ac3da0979b174a9433` 的 Live 写出和 REC 预览成功，**取景恢复断言失败**。该轮已经完成并保留原失败，没有上卡、USB 或刷机。

## 本轮实际结果

运行名 `v51d-final-pb-recovery`，QEMU `5ccbfc7e051a78dd24176aec98b3ab767709bc078dd3e179923c74b2f50a2ac2`。使用实际 v51d 头文件和 AArch64 构建参数重新生成 runtime offsetof；layout 来源、ELF 哈希与 driver 哈希在 `layout-provenance.json`。runtime 仍为23184字节（transaction workspace由指针引用），每次快照核对固件报告的size。该driver没有W5/hold偏移假设。

- Live：1次按下/1次writer返回/1次transaction，transaction_error=0，编码413帧。
- 实际 MOV：`recovery-failure-evidence/v51d-final-pb-recovery/DSC_0001.MOV`，916997字节，SHA256 `fdba7a341696ef9f7164c23dc00f4ef431806dd7b158fecc8b1f62ee1fed5ef9`。
- REC 预览：38帧，playing最终0，error/lookup_error均0，consumed_rec恰好增加1。
- 半按返回：IRQ一直为1584、编码一直为413；原断言 `No new Live frames/encoding after half-press return` 失败，trace_complete=false。最终截图显示原厂 `Error. Turn camera off and then on again.`。
- 输入未变，自己的QEMU已停止。`qemu-v51d-final-recovery.json`保留状态及导出文件收据。

## 错误发生的先后顺序

本次稀疏被动plugin在原厂两处入口观察寄存器，不改guest RAM、寄存器、MMIO或时钟。只在最终断言之后读取160字节错误历史。7165个事件没有环形覆盖，寄存器读取错误为0。

进入回放的 `step-114-pb-enter-release` 之后、`step-115-pb-view` 之前，发生：

1. `CaptureInformationService`入口`0x40ece93c`，x0..x3=`0x40,0,1,1`，LR=`0x40f53390`，host monotonic ns=`361513262898630`。
2. `ErrorInformationService`记录入口`0x41018a80`，x0..x2=`0xa2,0,1`，LR=`0x40eced94`，host monotonic ns=`361513263102855`。
3. REC触发在后面的`step-116`；因此这次原厂错误**先于Live自有预览启动**，不是返回时首次产生。Error对话框是在返回取景截图中看到的；不能把截图出现时间当作内部错误产生时间。

最终历史首条type=0xa2，与入口事件一致。事件时钟是主机monotonic；只据checkpoint定位发生区间，不把它换算为精确虚拟相机时刻。详见`qemu-v51d-stock-error.json`。进入回放前后、预览及返回截图、原始result、plugin文件、stderr与driver在`recovery-failure-evidence/v51d-final-pb-recovery/`。

静态确认：原厂表`0xa029d8280`的kind0x40条目为`[0x40,0xa2,0,0x10]`；表`0xa029d88a8`将外部码0xa2映射为kind0x40。记录函数把32字节历史存入`0xa03e04f44`，保存5条。尚未闭合此错误码的物理含义。相关反汇编在`error-service-disasm.txt`、`error-record-disasm.txt`、`error-callers-disasm.txt`。

原厂停取景控制的对比：成功v50进入回放时，在150C/1508停机和C0 ring-stop之后会出现`Z9_PRIMARY_BOUNDED_RESET`与C1 release；本次失败区间没有该reset。返回后失败轮没有新C0 START，1543保持inactive、starts=stops=3；成功轮出现第4次START并恢复raw帧。没有通过手动清除模型状态掩盖这一差别。

## 控制轮及归因边界

| 版本/运行 | 拍摄与预览 | 半按取景恢复 |
|---|---|---|
| v50 `v50-final-pb-recovery` | tx1，preview45 | 成功；IRQ1580→2074，编码421→667 |
| v50 `v50-recovery-control-r3` | tx1，preview39 | 失败；IRQ1594/编码428不增 |
| v50 `v50-pb-noobserve` | tx1，preview38 | 失败；IRQ1572/编码428不增 |
| v50 `v50-pb-error-observer` | tx1，preview45 | 成功；IRQ1558→2052，编码428→675；原厂错误入口零命中 |
| v51 `v51-final-pb-recovery-r2` | tx1，preview38 | 失败 |
| v51 `v51-final-pb-recovery-r3` | tx1，preview39 | 失败 |
| v51 `v51-pb-noobserve` | tx1，preview37 | 失败 |
| v51d `v51d-final-pb-recovery` | tx1，preview38 | 失败；REC前已记录0x40/A2 |
| 原版v49 `v49-pb-error-baseline` | 第二次进菜单即IRQ停在446，没有Live文件 | 未到目标回放路径；记录0x40/A2，不能记为恢复对照 |

另有v50 control r2的MCU ACK早期失败，以及v51第一轮首帧编码-24超时；均保留，未计入上表的完整恢复对照。原v46/v46r/v50旧driver在REC状态判定处超时的结果也原样保留：它错误地要求已经被Live消费的REC位仍传入原厂decoder。修正使用精确44字节预期并额外验证consumed_rec增加一次，未删除检测。

v50目前是两次完整成功、两次同型恢复失败，**最新一次成功不能覆盖失败**。有无额外W5只读诊断都复现过失败，因此那7行额外读取不是必要条件。归一化后的QEMU/RAW/profile/machine properties/ROM/probe输入相同，固件及overlay manifest不同。

这些结果不能把错误归于新writer：v50旧writer也会失败，原版v49更在没有Live写出时记录同型码，且最终v51d这次错误先于REC预览。它们也没有证明模型是唯一根因，或排除候选的更早初始化/内存/排程影响。11月4日c770 base无REC、无preview也出现过同类无C0重启的原厂错误，但当时QEMU哈希不同，仅作为历史线索。恢复子项仍未通过稳定回归。

## 集成审计

`candidate-v51d-review.json`独立审计最终整镜像：相对v50的86809个变化字节全部在31个明确允许区间；Live108136字节、PB35272字节、meta helper24字节无重叠；portrait、回放尺寸补丁保持，v50让路/音频runtime字节保持；v49撤字体和Preparing保持；manifest完整重放与v51d symbols一致。该静态通过不替代上面的恢复失败。
