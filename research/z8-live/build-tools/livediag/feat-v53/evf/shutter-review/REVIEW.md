# 快门音频 adapter 独立对抗审核

当前冻结 patch `de9f3c8532bcdfa5dc39b0388379c99f2699c7b9d9ac20dace52b8e4aeaa69b7`：**未发现剩余阻塞，适合继续完整链接和 QEMU；不是物理发声或刷机验收。** 9个文件均与 production/changed-files.json 哈希匹配。没有修改 portrait/audio 的源文件。

本轮发现并由原作者修复的3个问题：

1. worker 的 `now` 早于刚被SFX任务发布的 `began`，无符号相减把新请求立即判成2秒超时。HELD/NATIVE 两处加比较后，独立实际 ARM 验证未来1 tick 的开始时间不会误取消。
2. 旧tone完成→恢复麦克风期间新START成为HELD，而麦克风仅104入队、第二条post失败；新held被取消后遗留DONE+STARTING+driver4。旧DONE分支既不发送108又等待idle，普通音频清理也被busy返回跳过。独立实际ARM复现旧 `5d5ba6…` 无pause、backend1不动；修后 `8a7fa1…` 先走自身pause→STOPPING，等108 ACK才回IDLE。实际partial-start后端和新holder发布的完整组合由作者的31例native测试补齐；本独立测试针对其可达终态。
3. resume成功、首个新PCM前freeze了完整旧窗口时，新增bridge返回LAUDIO_STATE，被runtime当作音频失败去stop。独立实际C+ASan/UBSan反例得到rc=-3。修后冻结时跳过bridge，沿原冻结drain规则返回0；完整PCM缓冲逐字节保持，bridge标志保留。thaw后下一50ms块正常成为新history并清bridge，没有补造新的缺失前缀。

源码和原厂协议核对没有发现其他阻塞：

- accepted2004不释放音频；实际2005后仍要等native pending、SFX和driver idle。`played`表示提交给原厂dispatcher，不是物理已发声。
- wake使用稳定单一信封和generation，不会以旧wake释放新held；DONE→RESUMING和恢复后→IDLE均CAS，保留并发新START。
- held取消通过原回调/outer清理；已经交给native的声音只由原SFX任务发原停止请求并等真实done，不把入队成功当完成。
- 暂停保留PCM/mark/descriptor，stop使用已有108 ACK+idle+callback-drain；关闭、stock hold及部分重启失败不能提前free。
- 音频warm在受控暂停期间保持之前值，不会把cold capture提升为Ready；恢复后的50ms块先排除跨unmute数据，只为明确的暂停间隙补零。冻结后的缓冲不被补零或释放。

证据：`review-before.json`、`review-fixed.json`、`test_review.py`（actual ARM adapter + 原G协议，暂停/ACK为明确边界）；`test_freeze.c`、`backend-host.c`、`pause_helpers.inc`（actual C core/backend，只有section属性去除和native service边界）；`RESULT.json` 固定哈希和范围。测试未运行完整相机调度，未模拟下层真实DAC声压/时序。

另先前音频startup审查发现“GO已ACK失败被永久当作等待PCM”，作者已在 `f3465b…` 修正并包含于此冻结版；不是本次新未解决问题。
