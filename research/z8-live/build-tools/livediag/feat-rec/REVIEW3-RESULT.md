[codex] Turn completed.
**判决：不接受进入“完整链接＋最终镜像检查”。** 三项原问题已有实质修复，但新增的旧转发检查仍存在已复现的漏检，应先补齐。

**唯一确认的剩余缺陷：【中】检查器把“范围内存在正确转发目标”当成“入口会执行正确转发”。**

位置：[verify_link.py:104](/path/to/Nikon/build-tools/livediag/feat-rec/src-fix35/recorder/verify_link.py:104)、[verify_link.py:48](/path/to/Nikon/build-tools/livediag/feat-rec/src-fix35/recorder/verify_link.py:48)。

本次在内存中构造自洽的 ELF／MAIN 负例：

- 将 `live_open_wrap` 符号入口改为 `0x401e3c34`，该处指令是 `RET`。
- 将 patch site `0x406cd468` 同步改为 `BL 0x401e3c34`。
- 将该符号大小设为 `0x20`，覆盖位于 `0x401e3c50`、指向正确 stock open `0x4085b320` 的原转发指令。
- **完整 `verify()` 返回 PASS，20 个 patch sites。**

实际入口立即返回，根本不会执行 stock open；检查器却信任候选 ELF 的函数范围，并线性收集其中的分支目标，没有验证可达性。应验证真实转发路径，并拒绝这类错误入口／函数范围负例。

**这是检查器漏检的证据，不是当前真实固件已经存在错误跳转或不开机的证据。**

三项复核结果：

| 项目 | 核实结果 |
|---|---|
| 合并回调作废 pending | 原失败序列已修复。当前 AArch64 对象在比较事件值前，以 `LDAXR/STLXR` 消费 pending；事件 `3` 随后置 DONE、清 ready／adapter_gen，保留 claimed。后续 REC 无法按原序列复用身份。 |
| 旧 continuation 独立地址 | 原漏洞已修复。6 个 continuation、6 个旧转发符号均与 SHA 固定的基线分支吻合；12 组“错误符号＋一致错误分支”负例全部拒绝。但旧 wrapper 转发验证仍有上述漏检。 |
| 未 armed 轻量早退 | 已修复。零 arm 检查先于新增全保存和 C 调用；完整有效链为 **111 条指令、32 字节峰值栈**，所测无效指针／身份路径为 **14–107 条、32 字节**。恢复寄存器、NZCV、SP及调用者有效栈；释放后的保存区不恢复旧字节。 |

入口汇编另行核实：生成源码与生成器一致，重新汇编到内存的代码与现有对象一致；未发现新增 x16/x17 破坏、PC 相对寻址错误或早退落点错误。被覆盖指令、CBZ/TBZ 两条恢复路径及 continuation 均与基线吻合。共享 bring-up 没有重复完整保存，但拒绝后仍执行原 hand-back，**整个入口的成本不能表述为仅32字节栈**。

本次只读完成源码、对象反汇编和内存负例检查；未修改文件，无 USB、刷写、相机操作或 QEMU。真实全量链接、最终真实镜像和硬件运行仍未验证。
