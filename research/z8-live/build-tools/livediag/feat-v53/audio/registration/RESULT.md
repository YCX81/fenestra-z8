# v53 原厂 MOV 编号/登记只读研究（未接生产）

精确 v52 G MAIN ed1e8496fccde443412e527a252de5029a55c749053b9b046da42bfc454aa613。以下是原指令与局部 ARM 实验；没有改 capture/writer，没有调用相机或真实卡登记。原厂正常 callsite 具有状态前置条件，不能将任意输入反例解释成原厂正常录像必坏。

## 已闭合分配 ABI

原厂电影路径 407294c4 → 40866c4c，前者实际构造 40B plan：u32 count@0，两个最多 16B entry（u32 type@8+16i，u64 planned_size@16+16i）。ROM 440d16f0 对应 {5,7,5,5,5,5,6}，type5 为 MOV。

`40866c4c(w0 card0/1, w1 kind5, x2 flags[2], x3 date6, x4 plan40, w5 flag0, x6 out36)`：检查指定卡 slot a04822af8[card] 非空，持有 DCF lock a048229f0；只指定卡，不采用照片双卡分配策略。原厂 flags 是零初始化的两个 word；日期来自 408747d8。kind5 使用 movie filename preference 02001200（不是照片 02000602）。返回 04320080 成功，04320082 参数/缺卡。

结果是 u32 group_count@0，每卡 group stride12：u32 file_count@4+12g、u32 id[2]@8+12g。单 MOV 必须 group_count1/file_count1。虽然当前路径只需28B，原厂 caller 留36B并清零。ID内含card/directory/number/type；不要拼前缀或复用照片编号。

内部 40866800 → 4086f210 → 40870824 选目录和下一编号 → 4085ff9c 预留索引对象。第二卡或第二关联类型失败会40861004回滚已成功预留并清输出count。40866b18只为type≤4发额外0432002b；MOV跳过它。`reserve-tests.json` 实际执行 public 显式卡和 inner rollback，共6案例，输出canary正常；唯一编号选择与底层存储索引是明示stub，因此不是端到端分配通过。

## 文件 ref、路径、提交/取消

原厂4072911c/4072969c拿`4085a8e0(id)` ref，再`4085bfe0(file)`构造并返回 file+0x74 的32B路径结构。路径指针属于ref，保持ref直到收尾。后续`4085aa7c(file)`释放。file+0x70为状态；新预留为2，已提交为1。

`4085b320(file)`是原厂特殊流式创建，走402db210/service1b→402dd9a8(flags301)，不可直接与当前402dd4f8(flags101)的exclusive写入生命周期混用。`4085b24c`也用301创建；现阶段没有证明它具exclusive语义。

`4085c4bc(file,actual_size)`原生把record+0x18写size，状态2→1，再408647c4更新DCF/计数并在成功时发04320028(id,size)。该函数尾部返回unlock结果，不能单看其返回值判定DCF提交成功。唯一direct caller406cdae4是另一写入路径的预登记，尚不能直接当本项目完整commit契约。

`4085ac54(id)`取消仅在file state2/3/8处理（mask10c），然后释放其临时取得的ref；外层仍需释放自己的ref。committed state1不走取消，所以MAN最后写失败后的已登记清理不能仅remove_owned或ac54。正常已登记删除入口4085c424(id,0)在G已被PB delete hook接管；完整已登记删除/关联通知和card-generation边界尚未局部执行闭合。

## 不可直接使用的 import helper

原厂电影分段收尾407296dc调用`4085c2c0(file,closed_source_path,size)`，合法前置是新reserved state2；它将2→3，rename40863c74，再4085b430 mode0进入stat/DCF提交，最终state1。不是通用register(path)。

`test_import_close.py`用原G c2c0/b430/c524指令，明确stub锁/path/FS/DCF sink：合法state2下rename=-1/-28且stat=-1仍提交caller size，返回04320080；初始state1则返回相同success却不rename/commit（state变4）。这说明不能只看返回码，也不能无条件用于任意状态/跨卷路径。不宣称正常原厂调用必发生这些故障。

## 推荐下一步（未实施）

按指定照片card与generation reserve唯一MOV→验证out→取得ref/原厂路径→现有create_exclusive直接写DCIM目标并严格close/stat→确认完整DCF commit及readback→MAN最后提交。错误必须区分未预留、已预留未建文件、已建未提交、已提交但MAN失败；只能删除本任务所拥有文件/ID。仍需闭合exclusive fd与file状态的合法桥接、commit成功判据、已提交删除以及换卡generation。先做独立QEMU正常+失败注入，再考虑生产源。
