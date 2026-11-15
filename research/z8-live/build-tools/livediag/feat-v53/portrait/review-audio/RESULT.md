# v53 扬声器输出补丁更新复核

最终审查源码 SHA-256：`96cff4d12f9652effbbaf7ff1f690a85cdbc5a95b2bac4f0894e4c4878674daa`。本次更新未找到新增阻塞；保留真机发声/路由与整机回归边界。

## 已纠正的前一版缺陷

Audio Agent 的真实 A 回归发现：首次照片 REC 没经过电影 UI 初始化，`a033868d0` 仍为零，导致输出17音量为零。此前 412675da 的 592 组测试仅证明它与电影 UI 的构造结果一致，**不证明这个缓存已初始化，也不能把该零值认作用户静音偏好**。旧审查已保留为 `RESULT-cache-candidate.md`，不作为当前候选验收。

## 已纠正的 DMA 长度缺陷

前一版1×960000 B配置超过原厂Serial DMA的16位word计数：`bytes/4-1`截断后实际仅173568 B，约0.904秒，环也一直返回第一段。这不是MOV音频内容损坏，而是播放配置不能表达整缓冲。当前C改为5×192000 B，总960000 B和4秒最大PCM、至少1秒零尾不变。

本目录重新执行交付G的3种实际Serial构造：旧长度在三路均截断；192000均精确。原厂paired/unpaired ring各10次，5个地址按192000步长完整轮换并准确wrap。另实际执行原厂14→17→1a→15，确认1a确实把同一ring配置成count5/bytes192000，并收到START成功、driver3。证据`serial-block-tests.json`、`native-ring-tests.json`、`candidate-block-service.json`。下层任务队列为观察桩，未把它写成物理DMA或3秒真实声音验收。

## 新 getter 与线程/范围

音量函数把来源改为 `403ec398(02000110)`，stage8/取消/所有权流程不变。交付 G 的原厂电影入口 `40665840..85c` 本身调用这个 getter，再把结果存进旧缓存。

对该 ID 实际执行链为 `403ec398 ->40951228 ->410d5ec8 ->41012c84 ->4101553c`，ROM 记录 type2 读取 u16 `a03d47afc`，写入调用者栈上输出；组/ID/类型都经过原厂范围检查，失败默认0。它读取已初始化的 RAM 分发表/组 ready，不需要 GUI 当前电影页面；此成功 getter 路径没有投递任务、等待事件或 UI setter。测试只把分发表和组 ready 按初始化状态放入 RAM，其余 getter 指令原样执行。

新 AArch64 对照扩大为 **0..36 全部37级 × 16组连接状态 = 592组**。每次候选执行前强制旧 UI cache=0，以持久 pref 为唯一音量输入，结果与原厂电影构造一致；pref0保持静音，pref15等非零仍非零，pref和旧cache均未写回。报告 `profile-pref-arm.json`，反汇编 `pref-getter-disasm.txt`。

实际 callback/state machine 的完整 ASan/UBSan 回归与15个追加取消/迟到ACK/静音反例已针对新 SHA 重跑通过，见 `lifecycle-block.json`。保留前一报告中 stage8 分离 +28 回复位、超时 stage6 保留 PCM、停止确认才交还等结论。stage6超时仍不会自动恢复，未将这一安全保留状态称作用户可恢复验证。

没有改动 Audio Agent 源码或运行中的 QEMU；所有补充测试/报告只写本目录。真实声音、音量调整、插拔路由和麦克风重启效果需最终整机/硬件回归。

复核程序与报告只写独立review-audio目录；没有修改生产候选源码。早期缓存版与仅pref版报告分别保存在`RESULT-cache-candidate.md`和`RESULT-pref-candidate.md`，不得作为C完整验收。
