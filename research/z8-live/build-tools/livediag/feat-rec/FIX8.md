用中文回答。在已复审通过的 lamp-v4（src-fix35/recorder_patch.py、src-fix35/recorder/、recorder-build/，实验室链接 MAIN 5c29c9ee）上增加一个可开关实验项。禁止 USB、刷写、操作相机、QEMU。
背景（只读阅读）：../codex-w5/qemu-handoff-20261110/README.md、../codex-w5/qemu-nr-rec-20261110/README.md、../codex-w5/qemu-rec-after-open-20261110/README.md、../codex-w5/history/rea-153f67f-ack8-nr/report.md。QEMU 中 v3b 的 REC 抢占路径完整成功（S=0、OPEN(0)、出 MOV），唯一被原模型拒绝的是 Live 的 EXIT 尾段在 0x402f3184/0x402f3190 以 ID 0x4a/0x4b 调 0x40c2d884，于 0x40c2d8c4 向 0x0d154348 写 0x00800000/0x01000000（NR 端口复位），当时取景前端仍有在途包。假设：真机上取景活动时复位 NR 导致取景/录像链挂起并看门狗重启；USB Debug 暂停取景所以主机关闭后 REC 正常。
要求：
1. 新增 RAM 开关 skip_nr（放在 fr_state 现有保留字或新字段，arm 工具加 --skip-nr 写入，默认 0；与 arm 无关时也必须 0）。
2. 仅当 skip_nr=1 且当前 EXIT 由 Live 自己的关闭流程（w5_close/preempt 调用原厂 EXIT 0x402f3084 的那一次）发起时，跳过 ID 0x4a、0x4b 两次复位调用；其余全部原样（0x43–0x49 照常，原厂自身录像结束时的 EXIT 绝不受影响）。用 Live 关闭期间设置/清除的“live_exit_active”标记加调用栈/LR 核实归属，说明如何防止并发的原厂 EXIT 被误跳过。若 bring-up 后续有对应的解除复位（如 0x402f2ed8 附近），核对跳过置位后解除复位是否无害，并列出证据。
3. 灯码新增一个点或在 R 帧值里带一位表明 NR 是否被跳过（若可行），便于从视频确认实验项生效；不行就说明。
4. 未 armed / skip_nr=0 时行为与 lamp-v4 完全一致，门控成本不增加；hook 的原指令重放、恢复点、寄存器/NZCV/FPCR 保存照 lamp-v4 方式。
5. 更新 arm 工具、README（含上机两组对照流程：A 只武装灯码；B 武装灯码+skip_nr）、测试；运行全部测试，如实报告未验证项，特别是“跳过 NR 复位后原厂 bring-up 是否能正常初始化 NR”的风险。
