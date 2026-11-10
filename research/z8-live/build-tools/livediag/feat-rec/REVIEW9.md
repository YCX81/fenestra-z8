用中文做只读对抗性复审，不要修改文件；禁止 USB、刷写、操作相机、QEMU。FIX9.md 是要求，FIX9-RESULT.raw 是实现方自述。审查 v6：src-fix35/recorder_patch.py、src-fix35/recorder/、recorder-build/（含 linked-lamp-v6），基线 bg34.main。lamp-v4/v5 已复审通过（REVIEW7-RESULT.raw、REVIEW8-RESULT.raw），verify_link CFG 不完整是已知项。
须用对象反汇编或反例核实：
1. 不开机/死机风险优先：新增 D 阶段码插入点在关闭流程的哪些位置？这些位置是否持锁、关中断、在 ISR 或持有 Wave5/FS/close_lock，调用 DELAY+GPIO 是否安全；hook 重放、恢复点、寄存器/NZCV/FPCR 保存是否正确。
2. 插入的约 8 s 延时会不会改变关闭流程的超时判定（如 4 s busy、close_lock、等待 worker 5、FINISH 等），从而把原本的卡死变成别的失败，或让原本成功的路径失败？逐个超时列出与发码点的相对位置，评估诊断结果的可解释性。
3. REC 路径上是否已确实没有任何记录器文件 I/O；准备事务仍在且只在照片模式。
4. D 码编码（阶段号、NR 掩码、CRC）与解码器一致；与 R 互斥；未 armed 门控与 v5 一致。
5. 下一次上机：A（skip_nr=0）/B（skip_nr=1）各种 I/D/R/S/M 组合能证明什么，尤其“最后一个 D 阶段号”如何定位卡点。
只列有证据的缺陷并标严重度，最后给判决：接受/不接受进入完整链接和 QEMU。
