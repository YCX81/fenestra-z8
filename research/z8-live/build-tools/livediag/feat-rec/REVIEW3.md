用中文做只读对抗性复审。不要修改任何文件；禁止 USB、刷写、操作相机，不跑 QEMU。
REVIEW2-RESULT.md 是上次发现，FIX2.md 是修复要求。审查当前 src-fix35/recorder_patch.py、src-fix35/recorder/、recorder-build/ 及检查脚本，基线 bg34.main。
逐条核实三项是否真正修好（自己构造反例或反汇编验证，不要采信 README 或测试结果的说法）：
1. 合并回调后 pending 作废，后续 REC 不能写入旧会话；
2. verify_link.py 对全部旧 continuation 的地址独立于待验 ELF，错误符号＋一致错误分支的负例必须 FAIL；
3. 未 armed 拒绝路径在全寄存器保存和 C 调用之前，寄存器/NZCV/栈完全恢复，开机早期空指针安全，指令数与栈占用属实。
同时查修复引入的新回归，尤其是 hook 入口的汇编（破坏 x16/x17 或标志位、PC 相对重定位错误、早退后回到错误位置）——这些会造成不开机。
最后明确给出判决：接受/不接受进入“完整链接+最终镜像检查”，只列有证据的缺陷并标注严重度。
