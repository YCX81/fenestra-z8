用中文做只读对抗性复审，不要修改文件；禁止 USB、刷写、操作相机、QEMU。FIX7.md 是要求，ANALYZE2-RESULT.md 是设计依据。审查灯码诊断版：src-fix35/recorder_patch.py、src-fix35/recorder/、recorder-build/，基线 bg34.main。verify_link 的 CFG 不完整是已知项，不算缺陷。
须用对象反汇编或反例核实，不采信文档：
1. 不开机风险最高优先：新增 hook（0x402f303c、0x402f3068 及 bringup）的被覆盖原指令、CBZ 重放、PC 相对、续行地址、x16/x17、NZCV、FPCR/FPSR、栈对齐与恢复是否正确；未 armed 路径是否完全还原且不增重。
2. 这些 hook 执行时的任务上下文：DELAY 和 GPIO 调用在 0x402f303c/0x402f3068 是否安全（持锁、关中断、ISR？），0x402f303c 的调用方是否还有非 REC 路径，归属校验是否防止开机/拍照/回放发码。
3. 确认入口之后真的没有记录器文件 I/O，且 hand-back 函数体、close、超时未被改变。
4. 灯码时序与解码：同步头、Manchester、CRC 实现与编码表一致；S/M 的值真是原始 W0、绑定后 map[W19]；解码器能否把原厂约 1 秒长亮误判成有效帧；60fps 下半位 50 ms 是否够。
5. 下一次实验中，R/S/M 各种组合能证明什么。
只列有证据的缺陷并标严重度，最后给判决：接受/不接受进入完整链接和 QEMU。
