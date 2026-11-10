用中文回答。延续 FIX1.md 与 FIX1-RESULT.raw：采用“最小字节补丁，接受共享扩展标签也变为 28 号”的方案。不连 USB、不刷写、不碰相机、不运行 QEMU。
1. 写 c64label_patch.py：输入 MAIN（先校验目标原字节与 bg34.main 一致，不一致就拒绝），把 C-10…C-64 共享描述符（0x7e00..0x7f12 相关样式）的 0x21f 改为 0x229，输出 MAIN 与 changed 范围 JSON；列出所有被修改的字节（地址、旧值、新值）和所有会受影响的页面（state2、拨盘选择条，及其他用到该描述符的地方）。说明与 bootguard（0x401fe000 段及其补丁点）、Live payload（0x401e2000..0x401fff80）、NFP 查表（0x40a41000..）无重叠。
2. 评估 28 号下 `C-10`…`C-64`（及带星号形式若存在）能否放进 62 宽；给出依据。
3. 写 QEMU 验证步骤（win-lab imenu-c64 驱动，run_imenu.reference.py 为副本）：截图 state2 页面 C9/C10/C11 以及拨盘选择条 C10，前置条件（卡上需要 C10+ 槽位记录的做法）写清楚，可直接执行的命令行。
4. 在 bg34.main 上实际运行补丁，输出 patched MAIN 的 SHA 和 diff 校验。
