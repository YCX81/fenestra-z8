# v52 EVF Live 图标／REC 预览输出候选

状态：源码、AArch64 完整 PB 编译和定点原 core 指令测试通过；未连接相机，未做物理 EVF 验收。由 root 合入完整 v52 后做整机回归。

## 可证明的边界

用户硬件观察：刷 v51 后 EVF 内没有 Live 图标，EVF 回放不能查看 Live。LCD 图标正常。本目录没有新增硬件日志。

反汇编证据：

- `4039a0b4` 忽略 x0，只返回 `a03229ee0 + index*0x118`。原先 `(0,4)` 的 0 不是 LCD 选择参数。
- GUI root `+108/+10c/+110` 是 `(group, output, target)`。`4032586c` 原样转给 `4033e748`；后者第一参数要求 <=2，并经 `4033d8cc` 选择描述符。
- `403f1b18` 读 `a032bbef0`，唯一直接 setter 调用在 `4037f2a0` 设1。旧字段名 `native_graphics_current_device` 容易误导：这里实际提供 GUI group。
- `40398570` 读 `a03229eb0`，返回1或4；原厂多个 GUI 构造路径将该值写到 root `+10c`。输出表 `42aad5e0` 把1/2/4映射到不同的三份设备状态。
- `40340d64` backing rectangle、`403409d4` enabled、`403409a4` enable 的入口参数是 `(output,group,target)`；内部交换前两参数后才调用 `4033d8cc`。旧代码给 `(group,output,target)`，只在 `(1,1)` 时相等。
- 旧 PB/core attach 强制 `root+10c==1`，两份 `icon32_select_locked` 也拒绝 `t[1]!=1`，因此源码确实不支持原厂输出4。原厂 compositor 已用 mask7提交，不需要额外打开所有输出。

以上支持输出路由修复；没有据此声称已在硬件测到唯一失败点。PB view2 固定对象 `a0284a0e0` 未放宽：现有反汇编没有证明 EVF 需要不同对象身份。

## 实现

`evf.patch` 的7个文件以最终 v51d-work 为基线，不包含 audio/CFR/handoff 改动。

- PB `global_output.h` 提供共享输出适配：使用原厂当前输出 getter；3个 tuple adapter 纠正参数顺序；backing 查询拒绝旧输出，以触发原有 stale/unlink/re-attach 流程。
- 全零 root 仍用原厂 constructor。已初始化 root 只有 id7fdf、两原厂 draw callback 签名、parent0、且 child list为空，才允许改输出。通过原厂 `40325e0c` 更新公开和缓存矩形；保留整个 native list/cache，不重复初始化造成缓存泄漏。外部 root原样保留，非空own root返回BUSY。
- PB attach 使用新 helper，原有 view/state/tuple/viewport/epoch/foreign-owner限制保留。
- 两份 icon painter 接受 output1/4；所有权额外验证当前输出，切换后旧 attachment不得擦除或启用新输出。
- Live `overlay_target_off` 调用只读 profile 中的 adapter，保留 v47 target关闭后的重新挂接行为。
- `core_output_patch.py` 在 PB link 后应用40字节窄补丁：`401a38c8`单条B接共享 initializer；`401a337c..3384`三条指令调用output validator；`401deec0/ec8/ed8`三个profile指针换为adapter。每个地址严格比对 v51d 原字节；符号必须位于 PB 区间；没有双向链接依赖、没有新增 cave。

## 验证

- `global-output-tests.json`：19项 actual-source ASan/UBSan，包括 LCD→输出4→LCD、foreign完全不改、非空拒绝、保留listcache、尺寸无效和参数顺序。
- `icon-output-tests.json`：5项 actual icon header，验证output4进入正确 `(group1,output4,target4)` native选择，输出切换拒绝旧owner、尺寸不符拒绝绘图。
- `native-restore-tests.json`：原 detach 合同回归通过，包含先unlink、owner/state/tuple/viewport限制、失败unlink保留及不遍历foreign page。
- `core-output-tests.json`：将最终 v51d 原 core 指令、40字节补丁及实际完整 link PB helper放入 Unicorn。8项核心 attach/is_current和拒绝路径通过；输出4实际调用 native backing/enable mock参数为 `(4,1,4)`，切换后is_current立即为0。Native GUI/显示端点为 ABI mock，这不是物理面板测试。
- `build-tests.json`：PB10单元完整 `-Oz -Wall -Wextra -Werror` 编译/link，35872B（v51d35272B，+600B）；改动Live UI按原flags编译，text +36B。最终与audio/CFR合并后的空间由 root完整link验收。

远端私有目录 `/path/to/user/fxfix/v52-evf`；本地测试与反汇编文件均保留。`test_core_output.py` 直接读最终v51d MAIN，不重写该输入。core patch用于最终综合候选时必须从该候选的 PB manifest获取新symbols。
