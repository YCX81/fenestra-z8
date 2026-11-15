# V61 回放控制流宿主回归

运行：

```sh
python3 -B build-tools/livediag/feat-onemov/v61/tests/run_host.py
```

可用 `--source /绝对路径/pb-fix37` 测试指定源码。默认取 `v61/work/pb-fix37`，需要 Clang 与 ASan/UBSan。

`run_host.py` 从实际 `runtime.c` 提取连续的原函数文本，带 `#line` 原位置编译，并直接链接实际 `sync.c`；不重写 lookup、桥接、worker、按键或删除/保护逻辑。`results.json` 记录实际 runtime/header/sync SHA256、各提取片段 SHA256、编译命令和全部测试输出。

原厂函数经 `FN` 分发到宿主桩：文件仅存在于进程内存，时钟/GUI/调度器可控；DCF slot 表与节点使用进程内固定地址 `mmap`，因此 `nb_mask()` 的地址检查、置位与清位也是原代码。没有相机、USB、存储卡或真实媒体 I/O。

最终媒体版共 92 组通过。媒体消息桩为 48 字节，event 位于 +32、card slot 位于 +40；覆盖 22–31 十种生命周期事件、A/B 两方向、源卡与目标卡分别变化、无效 slot、相同地址节点被源卡新世代复用，以及目标卡移除但 4910 仍停留虚拟 MOV 时的有限清理。源卡变化时测试要求不清可能属于新索引的位；仅目标卡变化时要求可返回原 RAW 并清本会话新增位。

另有静态顺序检查：`pb_system_notify()` 必须先以 acq_rel 增加全局 `media_epoch`，再发布任一卡的 `card_epoch` 更新。该检查随每次复跑执行，记录在 `results.json.static_checks`；它不替代真实多任务并发验证。

路径桩按已确认原厂语义实现：`407b95e4` 忽略 ID 低四位、返回 group primary 路径，不能用它理想化地强制取 JPG。精确成员接口 `4085a8e0 → 4085c0d0 → 4085aa7c` 另行提供；释放 ref 后立即毒化路径存储，检查实现确实先复制、后释放。每例结束均检查引用平衡。

结果只证明被测控制流在这些显式后端条件下的行为；不证明真实 RAW 引用记录布局、跨卡原厂页面状态、解码器、媒体事件并发或真机时序。实际通过案例以 `results.json` 的 stdout 为准。
