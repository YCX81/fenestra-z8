# 8357634 竖屏合入结果

本地分支 `portrait-photo-layout`，commit `8357634600b1a31a9a74fc0c6d0db704a37a9f9b` 已核对。v49 两张视频 OSD 表已含原先相同的布局/属性修复，每张是 273 个旧矩形加 1 个 histogram 特例，因此不能直接套上游要求“第二张表为 stock”的脚本。

`apply_portrait.py` 以精确 SHA 固定 stock、v49 reference 及上游两个源码文件，只更新两张表各 274 个 portrait rectangle（共 548 处 8 字节），再更新 124 字节 histogram 原位指令。原有 portrait gate、第二表属性、其余字段全部保留；完整反向恢复检查保证改动限于清单。单独叠加 v49 产物 `fef32e06bec2b10995758641ec5c0e35bd691676a8529f3c3ffb73c9c9397e54`，实际改变 2409 字节；主 Agent 已合入集成 MAIN。最终整体 bootguard build ID 由集成器负责。

`histogram-machine.json` 是 ARM64 Unicorn 指令执行对照：1024 组合全部通过，960 组合保留原厂坐标，64 有效竖屏组合跳过横屏预设。执行了真实 `0x4038d658` 方向 getter；外围设置/预设/坐标服务为可控 stub，并故意破坏 caller-saved registers。此项不是完整 QEMU 屏幕或真机验证。

## 本次完整 QEMU 尝试

已并行运行独立 FIFO、独立虚拟卡的方向/视频测试，记录在 `qemu-attempts.json`。候选 MAIN 为主 Agent 的 `4cb5235fc709220f64af5400680653999d2a03b0d5a4821975e0f6a7b2d50977`。

- 基线 v49+portrait 和集成 v50，在普通 merged QEMU 切到视频后同样停在未建模读取 `0x0e200154`。
- 已用现有 mvq QEMU 的明确视频接口模型补上原实验使用的 `0:0:0:0` 合成输入；下一处停止为 `0x15080014`，PC `0x40c07c24`。
- 所有实际运行都只读确认 `portrait_gate=1`、screen suppression=0；不是忘记加载 portrait.wlm。传感器输入设为左竖，但读出的 native sensor classification=3，UI direction=0。后续 v51 的只读观测又确认 auto-rotate=1，因此也排除了原厂旋转开关关闭；方向输入到 UI 的链条仍未闭合。
- 因此不能把取得的横屏截图称为左右竖屏/直方图通过；未启用上游的合成直方图、强制 group0 等临时 workaround。
- 上游 README 报告自己的 U-live01 左竖直方图截图通过。本次 win-lab 缺它引用的 `sb11_video.py` 和完整私有 runtime，旧路径不存在。该历史声明没有算作本次集成回归。

`run_orientation.py` 从保留的 `run_movie_reference.py` 用 `make_runner.py` 生成，实际模式杆按键和 `orientation-input` 外设输入可重放。没有写相机 RAM、USB 操作、上卡或刷机。

后续 v51 MAIN `1b0ecc754467c893e37085653f286479f7ea9341d3c10653b28ab1aca93e782b` 的 124 字节直方图块与两张完整 OSD 表，均已与上述 v50 核对字节完全一致。直方图块 SHA-256 仍为 `ae609dcc1d07aa3e8334fad09af3910efb005fb131b5ce0344deebe77b2ef54a`。`v51-payload-audit.json` 又逐项核对全部 549 处计划写入均一致。

原厂设置值的定位依据：方向 getter `0x4038d658` 读取属性 `0x02001dcf`，经 `0x403ec398` → `0x410d5ec8` → `0x4101553c` 的属性表查找，ROM descriptor `0x44013fe0` 指向 4 字节存储 `0xa03d70940`；EVF 属性 `0x02001dce` 则指向 `0xa03d7093c`。v51 lane 对这些地址仅作读取，在左竖输入前后均看到 1 / 0；没有调用 setter 或直接写 RAM。

最终 `portrait-v51-a1` 同样在视频切换后停于未建模 `0x15080014`（PC `0x40c07c24`）。该轮已结束，最后证据已收进 `qemu-attempts.json`。不将它计为竖屏屏幕通过，也没有继续添加未经依据的模型。

最终v51d MAIN `aa6e94e0042fb445c666de73c0f6b9e7e569010b7993a3ac3da0979b174a9433` 已再次独立核对：全部549处计划写入均吻合8357634；124B直方图和两张111648B完整表的SHA与前次一致。报告在 `../writer/v51d-source-portrait-audit.json`。此为最终集成字节验证，没有把上述模型阻塞改记为竖屏屏幕通过。
