用中文做只读对抗性复审，不要修改文件；禁止 USB、刷写、操作相机、QEMU。FIX5.md 是这次改动要求，ANALYZE1-RESULT.md 是背景。审查 src-fix35/recorder_patch.py、src-fix35/recorder/、recorder-build/，基线 bg34.main。
重点，须用对象反汇编或反例核实，不采信文档：
1. 入口记录真的在 hand-back 之前，且写卡前后 hand-back 的参数寄存器、栈、NZCV 完全还原；写失败或返回错误时 hand-back 仍执行。
2. 未 armed 路径未变重。
3. 新扇区布局：偏移不溢出 2048 以上的新大小，预分配大小、decoder、arm.py 一致；准备成功前不会写 REC 扇区；旧版文件不被误解。
4. 拒绝记录逻辑：只有精确 LR 才写，整个 boot 最多一条，不会在开机/拍照/回放时写卡。
5. 下一次复现时，五种情况（未到 hook/门控拒绝/卡在 hand-back/hand-back 返回后出事/记录器 I/O 卡住）能否凭文件区分，列出每种对应的文件形态及无法区分的残余。
6. 死锁或卡死风险：REC 任务上下文下在 hand-back 前做同步写卡，是否可能与 Live worker/拍摄写卡争同一 FS 锁造成新的挂起。
只列有证据的缺陷，标严重度。最后给判决：接受/不接受进入完整链接和 QEMU。verify_link 的 CFG 证明不完整是已知项，不算缺陷。
