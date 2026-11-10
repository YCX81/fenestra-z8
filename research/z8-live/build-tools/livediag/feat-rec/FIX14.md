用中文回答。基于 v7（src-fix35/、recorder-build/，实验室 MAIN f7f9132d）做 v8。禁止 USB、刷写、操作相机、QEMU。
真机结果：v6（runs/a4991/report.txt）与 v7（runs/v7-4994/report.txt）都只解出 I 和 D1。v7 的 D1 显示 busy=0、alive=1。按代码，busy=0 时应立即发 D2（busy 循环不进入），但 D2、D3、D4、R 都没有出现，约 27 s 后重启。说明 MOVRS 任务在 D1 发完之后的极短代码区间里停止推进，或此后的发码失败/被遮挡（原厂在 REC 后会让卡灯连续亮约 1.3 s，可能遮挡）。在 hook 里继续同步发码已经无法再细分，且会扰动时序。
v8 设计（替代 hook 内的 D 发码）：
1. hook/关闭路径里：保留 I 码（证明入口）；之后所有阶段（包括 D1 之后的每一步：w5_diag 返回、alive 检查、release_req 写入、进入/每次 DELAY 前后、busy 循环每次迭代、超时、D2 位置、close 各阶段 10/11/13/15/17/18、EXIT 返回、notify 返回、S/M 位置）只写 RAM 进度字（单调阶段号 + 计数器 + 关键值，如 busy、alive、最近一次 DELAY 返回值、时间戳低位），不调用 DELAY、不碰卡灯。
2. Live worker 任务（runtime.c worker 循环，本身每轮 DELAY(5)）：会话 claimed 且 I 发完后，每约 2.5 s 用灯码报告一次 RAM 进度（阶段号 + 一个计数/值 + 一个“worker 心跳序号”），持续到重启。worker 是独立任务，若 MOVRS 卡住，worker 仍应运行；若 worker 也停发，说明全系统调度停止。请核实 worker 在照片切录像后仍在运行（photo=0 时循环照跑），以及在 worker 上下文调用 GPIO 0x40c15c3c 与 DELAY 是安全的（已有 lamp 调用历史可参考 v34/v35 的 lamp 代码）。
3. 灯所有权：I 码发完后灯由 worker 独占；I 期间 worker 不发。
4. 编码：新阶段号（与 I/R/S/M 区分），带 CRC；报告帧尽量短（半位 25 ms 可用，因为已用 120 fps），值字段足够承载进度阶段号（至少 6 位）与 2–3 位附加值；解码器同步支持；README 判定表更新。
5. 未 armed 成本不变；hook 重放、保存恢复、归属检查、硬截止逻辑保留；去掉的 D 发码不得留下持锁延时。
6. 更新测试并运行全部测试，如实报告未验证项，特别是 worker 在 REC 后是否被原厂挂起的风险。
