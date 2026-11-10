用中文回答。只修 REVIEW3-RESULT.md 列出的 verify_link.py 漏检，不改 recorder 运行代码、hooks 或 patch。禁止 USB、刷写、操作相机、QEMU。
要求：旧 wrapper 的转发验证不得信任候选 ELF 的函数入口/范围，也不得线性收集分支目标；须从固定入口按真实控制流走（遇到 RET、未知分支或超出固定路径即 FAIL），证明入口确实到达 SHA 固定的 stock 目标。
加入 REVIEW3-RESULT.md 中的自洽负例（live_open_wrap 入口改到 RET、patch site 同步、符号大小覆盖正确转发）并对每个旧 wrapper 各做一例，必须 FAIL；原有正例与负例继续运行。
完成后自我对抗：再尝试构造一个能骗过检查器的自洽 ELF，如实报告。如实列出通过项和未验证项。
