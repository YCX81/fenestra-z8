用中文回答。基于 v8b（src-fix35/、recorder-build/，实验室 MAIN 8866ea3b）做 v9。禁止 USB、刷写、操作相机、QEMU。
真机证据（runs/v8b-4996/report.txt）：REC 任务停在进度阶段 2（preempt_lamp 中 `w5_diag(w,1u,client,alive(w))` 调用前），始终没到阶段 3；Live worker 心跳 0→7 连续递增，约 23 s 后重启。
根因假设：struct live_w5 放在 pool() 分配的 DMA 块起始处（live_w5enc.c 约 345–356 行，w=(struct live_w5*)b，b 为 0x8xxxxxxxx 非缓存物理别名）。w5_diag 的 diag_n CAS、w5_close 与 preempt_lamp 的 close_lock CAS 编译为 LDAXR/STLXR（已在对象里确认）。在非缓存/Device 内存上独占访问可能永远失败 → 死循环、不让出 CPU → 看门狗重启。正常拍照路径从不执行这些 CAS；QEMU 总是成功；USB 主机关闭不经这些 CAS。
v9 要求（同时作为修复候选）：
1. struct live_w5 改用普通可缓存堆（与 runtime 相同的分配函数 0x40d2c408 或现有可缓存分配器）分配；DMA 用的 region[]/slot[] 继续放在 pool 块里，地址计算相应调整（不再在块头预留 struct 的页）。核实 struct 内没有任何字段被 VPU/DMA 通过物理地址访问（若有，把这部分留在 pool 并说明）。释放路径对应修改。
2. 全仓库排查其余对非缓存/pool/Device 内存执行 LDXR/STXR/CAS/fetch_* 的地方（capture、stream、audio、codec、recorder、runtime），列出并同样修正；对象级确认不再有独占指令作用于 pool 地址。
3. 保留 v8b 的 I 码与 worker 进度报告，进度阶段扩展到 close 全过程、notify 返回、S/M，便于看修复后是否走通或下一处卡点。
4. 未 armed 成本不变；hook 重放/保存恢复不变。
5. 运行全部测试，如实报告；特别说明：堆分配的大小、失败路径、照片模式 Live 拍摄行为是否改变。
