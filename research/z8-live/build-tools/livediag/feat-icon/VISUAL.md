# LIVE 第三版：窄体、第一列占位、双环

第三版画布 **110×44**（第二版 138×44），文字和图形白墨高度同为 **34 px**（原为 38 px）。保留 **5 px 主笔画、每侧 2 px 黑描边**，恢复“中心点＋实线内圈＋粗虚线外圈”。主稿为 [live-badge.svg](live-badge.svg)，由几何绘制，不是模型生成图。

本轮只修改 feat-icon 内 SVG、图像生成/检查脚本、geometry、说明及预览素材。没有修改固件源码、C 头文件或构建固件，没有 USB 或相机操作。`geometry.json` 保持 `preview-only; C headers not updated`。

## 三版及原厂像素对照

三列顺序为 **旧版 / 第二版 / 第三版**，每列上方是相同原厂 A、MF、S、FINE、VIEW＋相机裁片。全部统一最近邻放大 2×，没有独立拉伸字母。旧版和第二版直接读取归档 PNG。

![旧 / 第二版 / 第三版，统一 2×](icon32-style-comparison-2x.png)

本次重新生成的三张预览与 2× 对照图：

- [拍照与回放 1× 合成预览](icon32-visual-reference.png)
- [拍照左栏 2× 对齐预览](icon32-alignment.png)
- [三列 1× 样式预览](icon32-style-comparison.png)
- [三列 2× 对照图](icon32-style-comparison-2x.png)

![拍照与回放合成预览](icon32-visual-reference.png)

背景是既有 **1024×682 归档 QEMU framebuffer**，以整幅 framebuffer 为 viewport、1:1 贴入素材。不是本次相机实拍，也不证明 LCD/EVF 的实际显示倍率。

## FINE 每字宽高比与 LIVE 调整

测量来源是未叠加 LIVE 的 [拍照原帧](../../../_mac-migration/files/_bundle-20261102/codex-tasks/live-playback-ui/evidence/before-playback-shooting.png)，每次检查其 SHA-256：`2d42673b8809e4c3640ce04ef6565de3282599267dfd2f813fb29969b33bed3a`。

坐标为原帧像素，区间左闭右开。亮墨阈值为三通道均 ≥224；透明素材先计算 `RGB×alpha/255`，再取相同阈值。每个字母独立测包围盒，不用整词宽度除以字数。

| 原厂字母 | 白墨包围盒 | 字宽×字高 px | 宽/高 |
|---|---|---:|---:|
| FINE F | [800,77,819,116) | 19×39 | 0.48718 |
| FINE I | [827,77,831,116) | 4×39 | 0.10256 |
| FINE N | [842,77,861,116) | 19×39 | 0.48718 |
| FINE E | [871,77,890,116) | 19×39 | 0.48718 |
| MF M | [286,15,311,53) | 25×38 | 0.65789 |
| MF F | [318,15,337,53) | 19×38 | 0.50000 |

| LIVE 字母 | 第二版宽×高 | 第二版宽/高 | 第三版宽×高 | 第三版宽/高 |
|---|---:|---:|---:|---:|
| L | 19×38 | 0.50000 | 17×34 | 0.50000 |
| I | 5×38 | 0.13158 | 5×34 | 0.14706 |
| V | 23×38 | 0.60526 | 17×34 | 0.50000 |
| E | 19×38 | 0.50000 | 17×34 | 0.50000 |

L/V/E 的第三版宽高比相对 FINE F/N/E 差 **+2.63%**：34 px 字高按原厂比例应宽 16.56 px，取整为 17 px；与 MF 的 F 比例相同。V 的比例确实收窄；L/E 是绝对字宽减小，宽高比仍为 0.5，不宣称它们的比例也变窄。

**I 是明确例外**：保持 5 px 主笔画，单直干的字宽就必须是 5 px，无法同时达到 FINE I 的 4/39 比例。没有通过整体横向压缩把笔画变细。

为了让双环、窄体文字与固定 5 px 笔画一起靠近 VIEW 行边界，白墨高度从 38 微降至 34 px；仅把 V 收窄、继续保留第二版高度与大字距不足以贴近 VIEW 行。文字白墨间距由第二版 **7/8/8 px** 改为 **3/3/3 px**，图文间距保持 **4 px**。

L/I/E 在 y=16 的直干实测均 5 px；V 在 y=22 的两腿实测也均 5 px。V 的这里是水平截线值，斜边带抗锯齿。SVG 字母黑底沿轮廓扩 2 px；例如 L 的 y=16 右边黑色为 x=[46,48)，正好 2 px。完整测量见 [stroke-measurements.json](icon32-assets/stroke-measurements.json)。

## 总宽度与第一列边界

| 项目 | 白墨宽 px | 含黑边、非零 alpha 的可见宽 px |
|---|---:|---:|
| 原厂 VIEW＋相机 | 90 | 原帧无独立 alpha，未推算 |
| 旧版 LIVE | 94 | 97 |
| 第二版 LIVE | 131 | 135 |
| 第三版 LIVE | **103** | **107** |

第三版白墨比 VIEW 行宽 **13 px / 14.44%**；包括黑边的总可见宽 107 px，相对 VIEW 的 90 px **白墨参考宽**多 17 px / 18.89%。这两个口径分别报告，不把 VIEW 白墨宽当作包含描边的精确外轮廓。

第三版素材白墨框 `[3,5,106,39)`，含边缘抗锯齿的非零 alpha 框 `[1,3,108,41)`。拍照贴入点为 `(0,139)`：

- LIVE 外轮廓横向 **[1,108)**，完整落在第一列 [0,128)，右侧余量 **20 px**。
- VIEW 行白墨横向 **[9,99)**；LIVE 外轮廓左侧多出 8 px、右侧多出 9 px，大致对齐。
- LIVE 白墨中心 `(54.5,161)`，VIEW 白墨中心 `(54,97)`，水平差 0.5 px；行距仍 64 px。
- 回放贴入点仍为 `(19,20)`。

[geometry.json](icon32-assets/geometry.json) 的源/目标尺寸均为 110×44，拍照 `center_x=54`。预览按整数定位再约束至画面内；画布外围保持透明。几何配置尚未接入固件。

## 同心圆与外圈虚线

中心 `(20,22)`；图形白墨包围盒 34×34，与字高一致。

- 中心点：白色半径 3 px。
- 实线内圈：中心线半径 9.5 px，白环宽 **5 px**。
- 外圈：中心线半径 15.5 px，白色段宽 **3 px**，共 **12 段**，每 30° 放置一段。
- 每段跨 16°，中心线弧长 **4.33 px**；段间跨 14°，中心线空隙 **3.79 px**。白弧为平端，黑底向四周扩 2 px。弧面以 1° 小步长的多边形写入 SVG，避免不同渲染器的 dash 相位差异。
- 实际 PNG 在 y=22 横穿图形时，依次测得 **3 / 5 / 6 / 5 / 3 px** 白墨，对应外环、内环、点、内环、外环；白色层次之间保持黑色间隔。
- 沿外圈中心线逐角采样，确认存在 12 个独立白段，亮墨阈值下每段占 13–15°（抗锯齿使其小于几何 16°）。原尺寸也能看到分段，而非旧版的细碎点。

## 两项检查

```sh
python3 -B icon32_generate.py --assets-only
python3 -B icon32_check.py --assets-only
python3 -B icon32_visual_check.py --assets-only \
  --preview icon32-visual-reference.png \
  --alignment icon32-alignment.png \
  --comparison icon32-style-comparison.png
```

两项检查均 **PASS**，只运行图像路径，没有运行固件集成路径。

1. `icon32_check.py --assets-only`：两张静态 RGBA、48 张忙碌 PNG 逐像素核对；黑边和 alpha 不变；24×60 ms 的扫光峰值按点/L/I/V/E 顺序出现在第 **7/10/13/15/17** 帧，逐点验证渐亮、峰值、渐暗。
2. `icon32_visual_check.py --assets-only`：独立测量原厂与三版字宽高比、笔画和占位；L/V/E 比值相对 FINE 误差 <3%，全部主笔画 5 px；外圈 3 px、12 段分离；可见右边界 ≤128，左右相对 VIEW 偏差各 ≤10 px；图形与字高均 34 px；透明边界、深浅背景合成、SVG 动画、24 帧 GIF 解码核对全部通过。4-bit 白边覆盖率 RMSE **8.45**，二值参考 **63.68**（456 个边缘像素）。

[素材检查结果](icon32-assets/asset-check-results.json) · [视觉检查输出](icon32-assets/visual-check-results.txt) · [忙碌动画](icon32-assets/shoot-busy.gif) · [2× 扫光接触表](icon32-assets/sweep-contact-2x.png)

[旧版归档](icon32-assets/previous/) 和 [第二版归档](icon32-assets/second/) 均保留其 SVG、两张 PNG、geometry；本轮没有重绘历史版本。固件 C 源、头文件及 `.inc` 的前后 SHA-256 一致。
