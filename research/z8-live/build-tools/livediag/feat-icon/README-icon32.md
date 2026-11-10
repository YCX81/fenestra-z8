# Z8 Live 拍摄和回放图标

拍照和回放默认使用 **112×44 的透明底 LIVE 图标**。LIVE 字形按拍照界面 A、MF 的白色主笔画和黑色外缘重画，L、I 和 E 用平直的收笔。中心点、内圈和外圈各小点也有黑边，空隙透出取景画面。拍照将整个圆环和 LIVE 字样作为一组，放在 A 和 VIEW＋相机这两行下方。回放使用自己的位置 (24,24)/1280×800。两者按照当前目标的 viewport 定位。参考画面来自历史 1024×682 QEMU 输出，LCD 与 EVF 的实际面板仍需分别标定。

[与 A、MF 同画面对照](icon32-alignment.png) · [旧版和新版对比](icon32-style-comparison.png) · [原厂画面一比一合成对照](icon32-visual-reference.png) · [拍照静态 SVG](icon32-assets/shoot.svg) · [透明扫光 SVG](icon32-assets/shoot-busy.svg) · [测量依据与标定](VISUAL.md)

## 图标与切屏

- [可编辑 SVG 源图](live-badge.svg) 描述实心点、内圈、虚点外圈和 LIVE 笔画及其黑色描边。图形外的空白 alpha=0，黑色描边只覆盖轮廓，默认用多级覆盖率平滑内外缘。忙碌时白色笔画上的亮带从左到右扫过，保留黑色轮廓。严格二值显示可用 `--binary`。本机的 `FX_SCENE_IMAGE` 接收 RGBA 图像描述符，固件构建使用从同一 SVG 几何生成的掩码，按目标大小直接展开。
- **拍照对齐基准。** 在参考画面中，采用相同亮度阈值量取可见图形包围框，A 中线为 x=44，整组 VIEW＋相机为 x=54。默认以两者的中点 x=49 定位整枚 LIVE 的可见中线。每个目标尺寸分别量取整枚图形，按其中心定位，避免把 SVG 画布的透明留白算入。三行纵向中点约为 y=34、97、161。默认拍照 y 锚点为 98/480，目标宽度变化时重新按 viewport 算水平中线。空间不足时将图形夹在边界以内。其他输出上原厂列的位置仍需现场核对。
- 拍照 busy 从左端开始，每 60 ms 一个相位，24 相位一圈。正常的快门后取帧、等待 JPG、mux 和可选 DCIM 复制期间持续动画。worker 延迟时按实际时间取相位。回放单张 Live 照片显示静态徽标。
- 拍照在同一 attachment 原地更新并保留 readers/gate 保护。图标占用的矩形用于界定绘制范围，逐帧以 SRC 混合写入带 alpha 的图像，使灰阶边缘的覆盖率在重复绘制时保持稳定。若原厂改变 view、显示目标或 viewport，停止向旧目标呈现，仅尝试摘下自己的对象，再在新目标取几何、构图和呈现。正在绘制或摘除失败时保留原内存并重试。临时无法挂载时继续重试，图形后端报告图片错误时使用文字回退。忙碌动画沿用同一图层。
- 回放的 UI 任务会在静态徽标空闲时复查当前 owner、target 和 viewport，处理切到别的显示器、离开以及返回照片。徽标使用当前 viewport 的独立命令副本；播放中的 LVP 画面继续使用原有预览布局。姿态或原厂有效 UI 方向变化时重绘。旋转策略由原厂合成链决定，本层没有对物理姿态再叠加一次 90° 旋转。
- 已分别检查姿态单独改变、两种竖持、不同目标、viewport 偏移、临时失去 owner 与挂载/摘除重试。显示器保留了过期 target 时，模块等待原厂更新，避免覆盖其他所有者。最终合成角度、切屏空档和旧像素何时擦除，需要观察机身画面。

## 应用与调整

从本目录运行。补丁校验原输入的哈希，仅应用于工作副本。当前固件布局需要冷启动。物理机上的版本需要另行核对。
```sh
python3 -B icon32_generate.py
cp -R src-fix31 build-icon32-src
cp -R pbfix/live-playback-ui-icon build-icon32-pb
python3 v32_patch.py build-icon32-src
python3 -B icon32_patch.py build-icon32-src build-icon32-pb
python3 -B icon32_check.py
```

`--shoot-width/height/x/y` 与对应的 `--pb-*` 控制各页面。拍照默认用 `--shoot-center-x 49`（1024 宽参考）对齐整枚图形；显式传 `--shoot-x` 时采用左锚点模式。默认源图和目标尺寸相同。实测不同显示器的原厂文字大小若不同，可用 `--shoot-target` 或 `--pb-target` 按实测 device/type/target 三元组指定另一套源宽高。额外尺寸在生成时独立栅格化；未知目标回退本页默认尺寸。若极小 viewport 装不下，代码按比例收进边界。修改几何后在新的副本上应用、重新链接。[生成参数](icon32-assets/geometry.json) · [逐目标标定与参数示例](VISUAL.md)

## 与已有原厂图片查询入口共存

本目录 `bg34.main` 的 SHA-256 为 `c7c6dbeea8a94064a10d47a7b9022ab26e4aa6b12ba5fb63b1ad85abac4fe4d4`。按基址 `0x40160000` 解码，`0x4032dc64` 的指令 `0x141c4d0b` 已分支到 `0x40a41090`。已有代码按模式重映射一部分 ID，再用 `0x40a41400` 的 16 字节记录处理 `0x7000..0x7014`。表中 21 个 PNG 的 IHDR 依次给出 8 个 72×36、5 个 64×32 和 8 个 68×36。未命中会恢复 `mov x9,#0xcd58` 并跳回 `0x4032dc68`。这些尺寸属于资源，面板显示大小仍由布局决定。

外部项目 `z8log3g10/patcher.py` 分别接管录像 tile 的 ID 选择和图片查表，并让自定义 ID 得到私有 PNG。它要求查表入口仍为 `0xd299ab09`，它选用的 `0x1862+` 也只针对其输入。当前拍照和回放补丁使用各自已存在的 scene 图像路径，完全保留现有查表分支。将来若改为原生拍照控件，还需定位拍照 view 中负责显示、定位和失效重绘的调用者，再把新的私有图像与现有查询链合并。单独注册 PNG 没有创建控件或更新动画的作用。本次仅阅读外部源文件。

## 下次 USB 只读采值

各取一组可见徽标的拍照、回放单张照片状态。记录实际观察到的 LCD/EVF、横竖持和 USB 接入后的界面。每组在显示稳定时做两次独立采样。USB 如令拍摄 LV 暂停、屏幕切页或目标失活，丢弃该组；接入前的显示尺寸仍为未知。这里列出的值描述 graphics backing 与 viewport，不能直接给出面板物理分辨率。

所有读取为小端 RAM，指针应八字节对齐且整个读取落在 `[0xa00000000,0xa40000000)`。以下相对偏移按此 ABI 已交叉编译校验。

1. 从 `[0xa032b50d8]` 取 root，再从 `[root+0xf8f0]` 取 core。核对 core+64/+68 的 magic `0x465a394d`、installed，以及 core+32 的 u16 行数 46。
2. 从 `[core+2528]` 取 registry，校验 magic `0x47525846`、大小 64。拍照 UI 指针 U 在 registry+8，回放 P 在 registry+16。U 的 magic 接受旧 `0x4c505549/0x4c50554a` 与本版 `0x4c50554b`，P 的 magic 为 `0x50425549`。各版本按只读工具的尺寸检查。
3. 令 `D=U+12072` 或 `D=P+1856`，读取下表。当前 view 从 `[0xa032280c8]` 取指针 V。对本底本，`0x40393f6c` 和 `0x40393f28` 分别直接读取 V+0x54 的 ID 和 V+0xd0 的状态。

| 读取地址 | 类型和用途 |
|---|---|
| D+216、D+220 | u32 backing 宽高 |
| D+6032 | 四个 u16 viewport L、T、R、B |
| D+6056、D+200 | 三个 u32 的 device/type/target，以及 u64 surface。比对 surface+0x108 的三元组 |
| 前景平面描述符 +8、+10、+0x28、+0x3c | 两个 u16 宽高、u64 backing 值及 u32 格式编号。与显示对象的宽高分别记录；两份样本须属于同一 backing |
| 0xa02f35f50 | u64 通知函数地址；当前绘制路径接受 0 或 0x40325980 |
| D+224、D+228、D+240 | u32 attached、成功 present 计数、绘制错误 |
| D+6016、D+6024、D+6040 | u64 当前 owner view、u32 global_overlay、u64 gate 指针 |
| U+18144 或 P+32 | u64 gate；最低位必须启用，正常动画会推进 epoch |
| V+0x54、V+0xd0 | 两个 u32，拍照需 3/1，单张回放需 2/1 |
| 0xa03bd062c、0xa03bc8004 | u32 照片/录像选择器、姿态。拍照要求选择器为零 |
| 0xa03228244、0xa03228308 | u32 原厂方向输入和方向抑制 mask，用于与 posture/auto-rotate 状态对照 |
| U+0x4828、U+0x482c | u32 USB 状态及 USB 事件计数；两次间发生变化即弃用 |

[icon32_viewport.py](icon32_viewport.py) 接受已有 RAM 读取回调 `sample(read, "shoot")` 或 `sample(read, "pb")`。对本底本的 type=1、target=0..4，原厂函数 `0x4033d8cc` 的 RAM 查表基址 B 为 `0xa02f35f98`。device 0 的描述符为 B，device 1 为 B+0xb0+target×0x208，device 2 为 B+0xad8+target×0x208。工具直接读取这些字段并输出 `overlay_plane`，其他三元组在该字段返回空值。格式编号保留原值；面板上的透明度仍需观察。稍后接入 USB 时，在显示稳定后间隔数百毫秒用同一 `read(address,length)` 回调取两份，再调用 `compare(a,b)`。两个独立的连续 RAM dump 也可用于离线分析，默认首地址为 `0xa00000000`。

```sh
python3 icon32_viewport.py shoot first.bin --second second.bin
```

工具只调用提供的读函数，不打开 USB。单一 dump 可检查内部字段一致性；有两份独立采样才可比较目标变化。重复稳定读数也仅描述采样时的输出。若驱动或结构改版，须根据最终 ELF 重新导出偏移。

得到 viewport 后，令其宽高为 Vw、Vh。拍照横坐标先按 viewport 宽度换算列中线，再减去所选尺寸的墨迹中心偏移。回放及显式左锚点模式的 x 为 floor(anchor_x × Vw / 参考宽)，y 为 floor(anchor_y × Vh / 参考高)，最后将徽标夹在 viewport 内。默认宽高直接使用 112×44 backing 像素。OpenVG 后端负责加上 viewport 原点及坐标系翻转。分别记录从 LCD → EVF → LCD、横持 → 左/右竖持 → 横持、进入/退出回放的活动目标和相邻原厂图标亮墨。只有前后采样属于同一稳定状态时才用来标定。[面积覆盖测量](VISUAL.md)

## 离线验证

`icon32_check.py` 重新应用补丁，编译 AArch64 UI 对象并运行拍照、回放过渡、reader/gate、原厂摘除与只读采集用例。拍照常态、24 相位及循环首帧共 26 幅，逐像素对比通过。`icon32_visual_check.py` 独立解码素材与 GIF、量取原厂参考图并检查二值/灰阶量化。实际面板后处理、原厂调度与输出时机仍需实机证据。[测试结果](icon32-check/results.json)

`icon32_link.py` 以本地 MAIN 和匹配的挂钩清单为基准，分别链接完整的 AArch64 拍照与回放 payload，检查未定义符号、可写静态段及拍照 15 处、回放 10 处跳转的可达性。链接记录中的 `bytes` 和 `free_bytes` 给出各配置的占用和剩余字节数。更换或增加素材后须按实际配置复链，尤其要检查回放区域的剩余空间。复跑命令为 `../feat-rec/.recorder-venv/bin/python -B icon32_link.py`，先执行上面的 `icon32_check.py` 生成输入副本。这里生成离线 ELF 和验证记录。[链接记录](icon32-check/link-results.json) · [拍照 ELF](icon32-check/shoot-payload.elf) · [回放 ELF](icon32-check/pb-payload.elf)

验证构建对回放的 `ui.c` 和 `native_ui.c` 使用 `-Oz`，其余编译单元使用 `-Os`。这样默认尺寸和额外一组 144×56 素材都能放进已有的 40 KiB 回放区域。真实 target 的尺寸和位置待机身采值。

