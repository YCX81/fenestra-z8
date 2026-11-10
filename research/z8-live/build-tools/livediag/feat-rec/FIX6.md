用中文回答。按 REVIEW5-RESULT.md 只修下面两项，别的不改，同步 recorder-build/。禁止 USB、刷写、操作相机、QEMU。
A. 第 2 项：file_owned 只在准备事务完整成功（WRITE 字节足、CLOSE 返回 0）后才置位；准备失败后，REC 路径（入口、拒绝、后续事件）一律不写卡。增加“准备 WRITE 足够但 CLOSE 失败”回归，断言无后续写。
B. 第 3 项：fr_before 返回后、调用 live_w5_bringup_notify 前，恢复 x0–x7 与 NZCV，使 hand-back 看到与入口相同的参数寄存器和标志（只改 bringup() 这一段，SP 差异可保留，但报告实际深度）。
第 1、4 项是已接受的残余，不做处理；不要扩展 verify_link。
更新相关测试并运行全部测试，确认未 armed 仍是 111 指令/32 字节，如实报告未验证项。
