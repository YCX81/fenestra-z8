用中文回答。修改 Live 图标，让它和尼康原厂图标（同画面的 A、MF、S、FINE、VIEW）风格一致。只改 feat-icon/ 下的 SVG 源、生成器和预览，不要碰固件 src、不要构建固件，禁止 USB、刷写、操作相机。
用户反馈：现在的 LIVE 图标“和尼康其他 icon 不太像，太细了”。对照 icon32-visual-reference.png 和 icon32-alignment.png，我看到的问题：LIVE 字母笔画明显比 A、MF 细，字形偏窄；外圈虚点环在实际尺寸下几乎看不出；内圈笔画也细。
要求：
1. 从参考画面实测原厂字形 A、MF、FINE 的笔画宽度/字高比、黑色描边宽度、字间距，写进 VISUAL.md，并让 LIVE 字母按同样的笔画粗细与描边重画（粗、方正的无衬线，像 MF/FINE），字高与 A/MF 一致。
2. 圆环图形简化成在这个尺寸下清楚可辨的样式：保留 iPhone 实况“同心圆”的意思，但用实心点 + 一个粗实线环（可选第二个环用粗虚线，点要大到在实际像素下看得见），笔画粗细与字母一致，同样黑描边。整体高度与 A/MF 一致，图形与字母间距和 VIEW 图标的内部间距相当。
3. 忙碌扫光动画保留，在新字形上验证仍从左到右暗到亮。
4. 重新生成 icon32-alignment.png、icon32-visual-reference.png、icon32-style-comparison.png（旧 vs 新），并给出 2x 放大对照图，便于肉眼比较笔画粗细。尺寸 112×44 若需要调整说明理由并更新 geometry.json。
5. 运行 icon32_check.py / icon32_visual_check.py，报告笔画宽度实测（原厂 vs 新 LIVE）的数值对比。
