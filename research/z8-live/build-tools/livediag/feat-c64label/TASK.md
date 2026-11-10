用中文回答。只读调查 + 离线原型，不连 USB、不刷写、不碰相机；可在本目录写文件。
问题：用户在真机（当前固件 v36，MAIN 由 NFP v0.4 + C64 Picture Control 扩展槽层 + Live 组成；本目录 bg34.main 是 v31 同层次的 MAIN，C64 层相同，基址 0x40160000，SHA c7c6dbee）发现：i 键菜单里 Picture Control 图块，C1～C9 在图像下方显示具体编号，C10～C64 没有编号标志。
资料：C64 层的来源与工具在 ~/Nikon/_mac-migration/files/_bundle-20261102/（bridge64/fix-20261104/merge.py、slotcard/、slotcode/tools/z8_slots_imenu.py、z8_slots_image.py、z8_slots_layout.py、RESULT.md 等；z8_slots_imenu.py 注释写着“Native C-10 label resources are a separate required adapter; no host overlay or borrowed C-9 label is a valid substitute”）；Live 图标 README（../feat-icon/README-icon32.md）记录了：0x4032dc64 分支到 0x40a41090，已有代码按模式重映射部分 ID，再用 0x40a41400 的 16 字节记录处理 0x7000..0x7014，表中 21 个 PNG（8 个 72×36、5 个 64×32、8 个 68×36），外部项目 z8log3g10/patcher.py 接管录像 tile 的 ID 选择和图片查表。
任务：
1. 在 bg34.main 中查清 i 菜单 Picture Control 图块的绘制路径：C1～C9 的编号是图片资源（哪些 ID、尺寸、格式、存放位置）还是文字；C10～C64 现在显示什么、走哪条路径（slot 层 label_id 等）。给出反汇编证据和地址。
2. 设计给 C10～C64 加编号的方案，优先复用原厂/现有图片查询链（如 0x40a41400 表的扩展或新私有 ID），生成与 C1～C9 同风格、同尺寸、同格式的编号图（C10…C64，若 Picture Control 有多种基础样式×编号组合，说明组合方式与资源量和放置位置/空间）。不得借用 C-9 图。
3. 离线原型：从 MAIN 中提取 C1～C9 的原图，按相同字体风格生成 C10～C64 样张，输出对照预览 PNG（原 C1～C9 与新 C10～C64 并排），并写 PLAN.md：需要的补丁点、空间、与 Live/NFP/z8log 查表的冲突检查、QEMU 验证方法（参考 ~/.claude 记忆外的 imenu-c64 驱动说明：win-lab ~/.local/share/nikon-z8-qemu/imenu-c64/run_imenu.py）。
只写有证据的结论。
