# G 首次取景恢复失败：只读比较，冻结结论

结论：`v52g-hevc-close` 这一轮在首次编码器 OPEN 之前未恢复取景帧；不能把它计为 HEVC close/reopen 失败，也不能仅凭目前证据确定是固件还是 QEMU 的帧源/调度问题。与生产 G 的成功轮相比，重启控制写入确已发生，但原始帧和 Live IRQ 没有继续。保留这轮失败，不能用后续重跑覆盖。

证据来自已完成运行的 result、manifest、被动 observer 和 stderr；本次没有连接运行中的 GDB/QMP，没有修改固件、模型或驱动。可重放提取脚本为 `g-startup-comparison.py`，精简数据和原文件 SHA-256 为 `g-startup-comparison.json`。

| 项目 | 失败 `v52g-hevc-close` | 成功 `v52g-final-rec` |
| --- | --- | --- |
| 实际 `-kernel` SHA-256 | `5a8a306dc93e9853bed7a7c57a37e768245a1a61ddefe9e400edb9fbf68179f7` | `ed1e8496fccde443412e527a252de5029a55c749053b9b046da42bfc454aa613` |
| 运行结果 | `No verified Live photo owner for REC`；此前 2536 checks | 无 error，3110 checks |
| step 104 IRQ / raw frames | 464 / 232 | 460 / 230 |
| step 110 IRQ / raw frames | 464 / 232 | 560 / 280 |
| step 110 已编码 / encoder.hw | 0 / 0 | 63 / 非零 |
| step 115 IRQ / 已编码 | 464 / 0 | 1570 / 553 |

两轮使用相同 QEMU SHA `5ccbfc7e051a78dd24176aec98b3ab767709bc078dd3e179923c74b2f50a2ac2`、相同完整 `-M` 参数及相同 `run_live.py`。两轮均无 RAM patches 或 CPU register redirection，使用 instruction clock，原厂 timeout 未修改，非 cycle accurate。

驱动精确 SHA 为 `aea206673c327f1d3283c4a5f8e3e0526944da41af2a3aeb9931c21a1316a12a`。result 顶层 `firmware_sha256=2b43a31c…` 指原版输入镜像，不能用它代替实际 `-kernel` 身份；上表来自运行 manifest。

## 已证实的事件顺序

失败轮最后一个 Live completion 为 seq 7347，PC `401e21bc`，event `41070030`、arg1=1，IRQ 时间戳停在 44,207,317 μs。随后菜单停止路径正常结束：

- 所有 6 次 `4115fbbc` 等待均采到 mask `0x40`，`4115fbc0` 返回 1。
- 所有 2 次 `41155434` 等待均采到 mask `0x1000`，`41155438` 返回 1；stop 返回 0。
- 退出菜单的 seq 11661 已执行 `40c26848` reset，11670 到 `40c26804` start，11672 到实际 start 写入 `40c26810`，目标 `15431000`。
- 此后没有新 Live completion，也没有 401/402/403 编码器初始化观察点。step 110 的控制模型已为 ready=true、active=true、held=false、starts=3、stops=2，但 raw frames 仍为 232。
- worker 的 poll_count 从 step 104 的 11089 继续到 step 115 的 19404；requested=1、handoff_lock=0、encoder.hw=0、allocation_requested=0。这不是被 Live handoff 保持住或卡在 codec readiness guard 的迹象。

生产成功轮相应 start 事件为 seq 11637/11648/11650，随后 seq 11685/11686/11687 顺利通过 SET_PARAMS / 原厂 HEADER 命令 / HEADERS service，之后持续编码并完成 REC 回归。两轮的已记录停止等待均成功，不能重新套用先前 secondary-stop 缺完成事件的解释。

## 这轮确有另一条 A2 报告

seq 12833 在 classifier `40f5b2a0` 收到 x0=`0x100`、LR=`40f58474`；seq 12835 进入 `40ece93c`，kind=`0x40`；seq 12836 进入 `41018a80`，code=`0xa2`。生产成功轮未出现非零 classifier。

原厂指令 `40f5844c..40f58474` 显示：收到工作事件 bit23 后，读取 byte `a03bbbbc0`；非零则生成 `0x100`，调用 classifier，随后清该 byte。其顺序是：

```text
40f5844c  tbz w19,#23,40f58478
40f58460  ldrb w8,[x19]          ; x19=a03bbbbc0
40f58464  cmp w8,#0
40f58468  mov w8,#0x100
40f5846c  csel w0,w8,wzr,ne
40f58470  bl 40f5b2a0
40f58474  strb wzr,[x19]
```

因此这条 A2 来自另一原厂异步错误旗标，不是已采到成功返回的两个 stop wait。现有 observer 没有采这个 byte 的置位者，尚不能证明该 A2 是断帧的最初原因还是之后的超时结果。

## 明确未知与验证边界

raw 模型曾显示 wait_bank=true，后来变 false 而 raw frames 仍不前进；单个 wait_bank 快照不能确定缺哪一个输入事务。CPU/设备快照是在驱动暂停检查时取得，也不能把 QOM stopped 状态解释为意外 CPU 停机。没有足够证据认定具体模型缺口、原厂错误旗标的生产者或固件竞态。

根 Agent 后续报告：相同 Gclose 镜像及驱动原样重跑完整通过 3137 checks，实际 hw/VDI/worker 都归零、client4=ff 后，第二次 401/403/402 均返回 0，返回后 encoded 552→844、最终 905。这是独立后续运行的结论，其原始证据由根 Agent 归档；它不解释或抹掉本轮断帧。生产 3110 checks 则已由本报告读取完成结果确认。

本报告在此冻结：不扩大模型修复范围，不放宽原有断言，原首次取景恢复失败的根因仍未知。
