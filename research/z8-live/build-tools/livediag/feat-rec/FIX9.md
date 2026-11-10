用中文回答。在 lamp-v5（skip_nr 版，已复审通过，实验室 MAIN 636fbd16，源码 src-fix35/、recorder-build/）上做 v6。禁止 USB、刷写、操作相机、QEMU。
真机结果：A 组（skip_nr=0）解出有效 I=0，之后约 28 秒无 R/S/M，REC 后 28 秒重启；卡文件 W5L00004 有事件 15。B 组（skip_nr=1）视频无任何有效码，REC 附近卡灯连续亮 1.6 秒，卡文件 W5L00005 只有准备记录，事件 15 未落卡——推断卡在入口写卡（与原厂/Live 写卡争 FS 锁），B 组无效。见 runs/a4989/report.txt、runs/b4990/report.txt、runs/W5L0000{4,5}-v5.BIN。
要求：
1. 去掉入口事件 15 的写卡（REC 路径上不再有任何记录器文件 I/O；准备事务保留以确认武装）。I 码在入口精确 LR 匹配、锁定会话后立即发出。I/R/S/M 和 skip_nr 逻辑不变。
2. 新增“卡住报告”：会话开始后若 N 秒（建议 5 s）内未发出 R，由一个不被 hand-back 阻塞的执行上下文用灯码发出当前关闭阶段，然后每隔约 8 s 重复一次直到重启。阶段来源用关闭流程已有的 RAM 诊断（w5_diag 阶段号、close_lock、live_exit_active、skip_nr 掩码、worker 状态、slot/core busy、VDI 等，挑最能定位的 1–2 个字段编码）。请：
   a. 找出可用的执行上下文（例如 Live worker 自身是否会被 close 阻塞？原厂周期定时器/其他常驻任务的 hook？），给出证据它在 hand-back 卡住时仍会运行，且可安全调用 DELAY 和 GPIO；若找不到安全上下文，说明并改用其他方案（例如在 close 内部各阶段之间发极短的阶段码）。
   b. 新阶段码使用新的阶段编号（与 I/R/S/M 区分），带 CRC，解码器同步支持。
   c. 与 hand-back 正常返回发 R 的路径互斥，避免两路同时驱动卡灯。
3. 未 armed 成本不变，hook 重放与保存恢复照旧；更新 arm 工具、解码器、README（含上机 A/B 流程）、测试，运行全部测试，如实报告未验证项。
