用中文回答。撤销 v9 把整个 struct live_w5（约 1 MB）改到缓存堆的做法，改为最小改动版 v9b。禁止 USB、刷写、操作相机、QEMU。背景：FIX16.md、FIX16-RESULT.raw、runs/v9/REPORT.md。
要求：
1. struct live_w5 恢复为 v8b 的 pool 块头布局（地址计算、释放路径与 v8b 一致）。
2. 只把被原子 RMW 的字段（diag_n、close_lock，以及排查到的其他任何对 w-> 成员的 CAS/fetch_*/exchange）移到可缓存内存：放进 struct live_runtime（runtime.c 里由 0x40d2c408 分配的那块，记录器在其中的 CAS 已在真机上正常工作），通过 live_w5 中保存的指针或 runtime 访问；或用 memory_order 等价的非 RMW 设计（单写者）替代，并论证正确性。
3. 对象级确认：编译后 live_w5enc.c 及其他单元中，所有 LDAXR/STLXR/LDXR/STXR/CAS 指令的目标地址都不在 pool（0x8xxxxxxxx）内——列出每处独占指令及其基址来源。
4. 保留 v8b 的 I 码与 worker 进度报告及扩展的 close 阶段；未 armed 成本不变；hook 重放、保存恢复不变；照片模式 Live 行为不变。
5. 运行全部测试，说明“历史 reduced-ring 冻结帧数断言失败”在 v8b 上是否同样失败（是否既有失败），如实报告，并给出与 v8b 的源码 diff。
