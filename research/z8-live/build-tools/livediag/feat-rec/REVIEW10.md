用中文做只读对抗性复审，不要修改文件；禁止 USB、刷写、操作相机、QEMU。REVIEW9-RESULT.md 是上轮三项缺陷，FIX10.md 是修复要求，FIX10-RESULT.raw 是实现方自述。审查 src-fix35/recorder_patch.py、src-fix35/recorder/、recorder-build/，基线 bg34.main。
用对象反汇编和自建反例逐条核实三项是否真正修好（不采信测试或文档），并重点查修复引入的回归：
- 竞争者宽限是否可能变成永久等待或过长等待，未 armed 是否完全不变；
- 调用者归属检查是否会误拒真正的 REC 会话（例如关闭在另一个栈/任务里执行时）而导致关键 D 码丢失；
- VDI 计时与 v5 等价；其他超时起点未变；
- hook 重放、寄存器/NZCV/FPCR 恢复、未 armed 门控 111 指令/32 字节仍成立。
只列有证据的缺陷并标严重度，最后给判决：接受/不接受进入完整链接和 QEMU。verify_link CFG 不完整是已知项。
