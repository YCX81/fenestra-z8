用中文回答。在本目录工作，不连 USB、不刷写、不碰相机、不运行 QEMU（QEMU 由我在实验室跑）。
用户真机照片（evidence/img_5008.jpg、evidence/img_5009.jpg）确认：问题在 i→Picture Control→OK 后的 state2 页面，C1～C9 示例图下方有 `C-x` 文字，C10 起示例图下方一行完全空白（选中时黄色背景里也无字）。PLAN.md 第 2 节已发现：扩展字符串使用字体样式 0x21f（尺寸 36），原厂为 0x229（28），文字框 62×28。
任务：
1. 找出 C64 扩展层中给 C-10…C-64 文字指定样式 0x21f 的确切位置（代码立即数或数据表），用反汇编/字节证据说明它只影响扩展行，不影响原厂 C1～C9 或其他页面。
2. 写 c64label_patch.py：输入一个 MAIN（必须先校验目标位置原字节与 bg34.main 一致，不一致就拒绝），输出改为 0x229 的 MAIN 与 changed 范围 JSON；只改这几个字节，不扩大字框、不加 PNG。与 bootguard（0x401fe000 段、0x401c4d4c 等）、Live payload（0x401e2000..）、NFP 查表（0x40a41000..）无冲突，说明理由。
3. 评估 `C-10`…`C-64` 以及可能带星号的字符串在 28 号字下是否放得进 62 宽：从 MAIN 中找原厂字形宽度表或用已知 C-1…C-9 的宽度推算；若有风险，列出备选（例如去掉连字符）但先不实施。
4. 写 QEMU 验证步骤：用 win-lab imenu-c64 驱动（run_imenu.reference.py 是副本）进入 i→PC→OK 页面并翻到 C10～C11 截图所需的按键序列与 slot 前置（需要卡上有 C10+ 的 NP3/槽位记录，参考记忆：card-scheme keys、z8_slots_layout.py）。
