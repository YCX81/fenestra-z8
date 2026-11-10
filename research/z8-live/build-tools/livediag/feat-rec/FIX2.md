用中文回答。按 REVIEW2-RESULT.md 修复 armed-only 飞行记录器的三项缺陷，只改 src-fix35/recorder_patch.py、src-fix35/recorder/ 并同步到 recorder-build/。禁止连接 USB、禁止刷写、禁止操作相机，不跑 QEMU。

1. 旧 pending 跨 REC 复用：携带启动位但未被接受关联的 adapter 回调（如合并事件 3），必须原子消费/作废 pending 并终止该诊断会话，不能在外层门控直接返回。之后的 REC 不得把完成记录写入前一会话。增加“合并回调→下一次 REC”回归用例。
2. verify_link.py 漏检：独立固定全部旧 continuation（包括 live_irq_resume=0x40e9083c、live_start_resume=0x40c26808、live_closed_resume=0x408b7c20 及其余所有旧 hook），不得信任待验 ELF 的符号值；地址必须从基线反汇编或现有补丁源独立得出，并核验旧转发目标。增加“错误 ELF 符号＋对应错误分支彼此一致”的负例，必须 FAIL。
3. 未 armed 轻量早退：零 arm 检查放到全寄存器保存和 C 调用之前，只用少量明确保存的临时寄存器完成只读查找；全量保存仅走已启用路径。所有查找必须对空指针/未初始化安全（开机早期也会命中）。验证拒绝路径的寄存器、NZCV、栈完全恢复，并报告拒绝路径的实际指令数和栈占用。共享 bring-up 不要重复做完整保存。

修完运行全部现有测试与新增测试，如实报告通过项与未验证项。不要声称已完成真实全量链接或硬件验证。
